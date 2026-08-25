#include "rblink_usb.h"
#include "rblink_uart.h"
#include "rblink_board.h"
#include "usb_otg.h"
#include "usbd_core.h"
#include "usbd_ctlreq.h"
#include "DAP.h"

#define RB_DAP_OUT_EP       0x01U
#define RB_DAP_IN_EP        0x81U
#define RB_CDC_CMD_EP       0x82U
#define RB_CDC_OUT_EP       0x03U
#define RB_CDC_IN_EP        0x83U
#define RB_USB_MPS          64U
#define RB_MS_VENDOR_CODE   0x20U

static USBD_HandleTypeDef usb_device;
static uint8_t dap_rx[RB_USB_MPS] __attribute__((aligned(4)));
static uint8_t dap_tx[RB_USB_MPS] __attribute__((aligned(4)));
static uint8_t cdc_rx[RB_USB_MPS] __attribute__((aligned(4)));
static uint8_t cdc_tx[RB_USB_MPS] __attribute__((aligned(4)));
static volatile uint8_t dap_pending;
static volatile uint8_t dap_tx_busy;
static volatile uint8_t cdc_tx_busy;
static uint8_t cdc_control_pending;
static uint8_t interface_alt;
static uint8_t cdc_line[7] = {0x00U, 0xC2U, 0x01U, 0x00U, 0U, 0U, 8U};

static uint8_t class_init(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t class_deinit(USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t class_setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req);
static uint8_t class_ep0_rx(USBD_HandleTypeDef *pdev);
static uint8_t class_data_in(USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t class_data_out(USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t *class_config(uint16_t *length);
static uint8_t *class_qualifier(uint16_t *length);

static USBD_ClassTypeDef rblink_class = {
  class_init, class_deinit, class_setup, 0, class_ep0_rx,
  class_data_in, class_data_out, 0, 0, 0,
  class_config, class_config, class_config, class_qualifier
};

/* Interface 0: CMSIS-DAP v2 vendor Bulk. Interfaces 1/2: CDC ACM. */
__ALIGN_BEGIN static uint8_t config_desc[] __ALIGN_END = {
  0x09, USB_DESC_TYPE_CONFIGURATION, 0x62, 0x00, 0x03, 0x01, 0x00, 0x80, 0xFA,
  0x09, USB_DESC_TYPE_INTERFACE, 0x00, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x05,
  0x07, USB_DESC_TYPE_ENDPOINT, RB_DAP_OUT_EP, 0x02, 0x40, 0x00, 0x00,
  0x07, USB_DESC_TYPE_ENDPOINT, RB_DAP_IN_EP,  0x02, 0x40, 0x00, 0x00,
  0x08, USB_DESC_TYPE_IAD, 0x01, 0x02, 0x02, 0x02, 0x01, 0x00,
  0x09, USB_DESC_TYPE_INTERFACE, 0x01, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
  0x05, 0x24, 0x00, 0x10, 0x01,
  0x05, 0x24, 0x01, 0x00, 0x02,
  0x04, 0x24, 0x02, 0x02,
  0x05, 0x24, 0x06, 0x01, 0x02,
  0x07, USB_DESC_TYPE_ENDPOINT, RB_CDC_CMD_EP, 0x03, 0x08, 0x00, 0x10,
  0x09, USB_DESC_TYPE_INTERFACE, 0x02, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
  0x07, USB_DESC_TYPE_ENDPOINT, RB_CDC_OUT_EP, 0x02, 0x40, 0x00, 0x00,
  0x07, USB_DESC_TYPE_ENDPOINT, RB_CDC_IN_EP,  0x02, 0x40, 0x00, 0x00
};

__ALIGN_BEGIN static uint8_t qualifier_desc[] __ALIGN_END = {
  0x0A, USB_DESC_TYPE_DEVICE_QUALIFIER, 0x00, 0x02, 0xEF, 0x02, 0x01, 0x40, 0x01, 0x00
};

/* Microsoft OS 2.0: bind interface 0 to WinUSB without a custom INF. */
__ALIGN_BEGIN static uint8_t ms_os_20_desc[] __ALIGN_END = {
  0x0A,0x00, 0x00,0x00, 0x00,0x00,0x03,0x06, 0x2E,0x00,
  0x08,0x00, 0x01,0x00, 0x00,0x00, 0x24,0x00,
  0x08,0x00, 0x02,0x00, 0x00,0x00, 0x1C,0x00,
  0x14,0x00, 0x03,0x00, 'W','I','N','U','S','B',0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

__ALIGN_BEGIN static uint8_t bos_desc[] __ALIGN_END = {
  0x05, USB_DESC_TYPE_BOS, 0x21,0x00, 0x01,
  0x1C, 0x10, 0x05, 0x00,
  0xDF,0x60,0xDD,0xD8, 0x89,0x45, 0xC7,0x4C,
  0x9C,0xD2,0x65,0x9D,0x9E,0x64,0x8A,0x9F,
  0x00,0x00,0x03,0x06, 0x2E,0x00, RB_MS_VENDOR_CODE, 0x00
};

static uint8_t class_init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  (void)cfgidx;
  (void)USBD_LL_OpenEP(pdev, RB_DAP_IN_EP, USBD_EP_TYPE_BULK, RB_USB_MPS);
  (void)USBD_LL_OpenEP(pdev, RB_DAP_OUT_EP, USBD_EP_TYPE_BULK, RB_USB_MPS);
  (void)USBD_LL_OpenEP(pdev, RB_CDC_CMD_EP, USBD_EP_TYPE_INTR, 8U);
  (void)USBD_LL_OpenEP(pdev, RB_CDC_IN_EP, USBD_EP_TYPE_BULK, RB_USB_MPS);
  (void)USBD_LL_OpenEP(pdev, RB_CDC_OUT_EP, USBD_EP_TYPE_BULK, RB_USB_MPS);
  pdev->ep_in[1].is_used = 1U; pdev->ep_out[1].is_used = 1U;
  pdev->ep_in[2].is_used = 1U;
  pdev->ep_in[3].is_used = 1U; pdev->ep_out[3].is_used = 1U;
  dap_pending = dap_tx_busy = cdc_tx_busy = 0U;
  (void)USBD_LL_PrepareReceive(pdev, RB_DAP_OUT_EP, dap_rx, sizeof(dap_rx));
  (void)USBD_LL_PrepareReceive(pdev, RB_CDC_OUT_EP, cdc_rx, sizeof(cdc_rx));
  return USBD_OK;
}

static uint8_t class_deinit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  (void)cfgidx;
  (void)USBD_LL_CloseEP(pdev, RB_DAP_IN_EP); (void)USBD_LL_CloseEP(pdev, RB_DAP_OUT_EP);
  (void)USBD_LL_CloseEP(pdev, RB_CDC_CMD_EP);
  (void)USBD_LL_CloseEP(pdev, RB_CDC_IN_EP); (void)USBD_LL_CloseEP(pdev, RB_CDC_OUT_EP);
  RB_UART_SetControlLines(0U);
  return USBD_OK;
}

static uint8_t class_setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
  if (((req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_VENDOR) &&
      (req->bRequest == RB_MS_VENDOR_CODE) && (req->wIndex == 0x0007U)) {
    uint16_t n = (req->wLength < sizeof(ms_os_20_desc)) ? req->wLength : sizeof(ms_os_20_desc);
    (void)USBD_CtlSendData(pdev, ms_os_20_desc, n);
    return USBD_OK;
  }
  if ((req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD) {
    if (req->bRequest == USB_REQ_GET_INTERFACE) {
      (void)USBD_CtlSendData(pdev, &interface_alt, 1U);
      return USBD_OK;
    }
    if ((req->bRequest == USB_REQ_SET_INTERFACE) && (req->wValue == 0U)) {
      interface_alt = 0U;
      return USBD_OK;
    }
  }
  if (((req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_CLASS) &&
      ((req->wIndex == 1U) || (req->wIndex == 2U))) {
    switch (req->bRequest) {
      case 0x20U: /* SET_LINE_CODING */
        cdc_control_pending = 0x20U;
        (void)USBD_CtlPrepareRx(pdev, cdc_line, 7U);
        return USBD_OK;
      case 0x21U: /* GET_LINE_CODING */
        (void)USBD_CtlSendData(pdev, cdc_line, 7U);
        return USBD_OK;
      case 0x22U: /* SET_CONTROL_LINE_STATE */
        RB_UART_SetControlLines(req->wValue);
        return USBD_OK;
      case 0x23U: /* SEND_BREAK */
        return USBD_OK;
      default:
        break;
    }
  }
  USBD_CtlError(pdev, req);
  return USBD_FAIL;
}

static uint8_t class_ep0_rx(USBD_HandleTypeDef *pdev)
{
  uint32_t baud;
  (void)pdev;
  if (cdc_control_pending == 0x20U) {
    baud = (uint32_t)cdc_line[0] | ((uint32_t)cdc_line[1] << 8U) |
           ((uint32_t)cdc_line[2] << 16U) | ((uint32_t)cdc_line[3] << 24U);
    RB_UART_SetLineCoding(baud, cdc_line[4], cdc_line[5], cdc_line[6]);
    cdc_control_pending = 0U;
  }
  return USBD_OK;
}

static uint8_t class_data_in(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  (void)pdev;
  if (epnum == (RB_DAP_IN_EP & 0x7FU)) dap_tx_busy = 0U;
  if (epnum == (RB_CDC_IN_EP & 0x7FU)) cdc_tx_busy = 0U;
  return USBD_OK;
}

static uint8_t class_data_out(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  uint32_t count = USBD_LL_GetRxDataSize(pdev, epnum);
  if (epnum == (RB_DAP_OUT_EP & 0x7FU)) {
    if (count != 0U) dap_pending = 1U;
  } else if (epnum == (RB_CDC_OUT_EP & 0x7FU)) {
    (void)RB_UART_Write(cdc_rx, (uint16_t)count);
    RB_LED_Data(1U);
    (void)USBD_LL_PrepareReceive(pdev, RB_CDC_OUT_EP, cdc_rx, sizeof(cdc_rx));
  }
  return USBD_OK;
}

static uint8_t *class_config(uint16_t *length) { *length = sizeof(config_desc); return config_desc; }
static uint8_t *class_qualifier(uint16_t *length) { *length = sizeof(qualifier_desc); return qualifier_desc; }

void RB_USB_Task(void)
{
  if (dap_pending && !dap_tx_busy) {
    uint32_t result;
    dap_pending = 0U;
    result = DAP_ExecuteCommand(dap_rx, dap_tx);
    dap_tx_busy = 1U;
    if (USBD_LL_Transmit(&usb_device, RB_DAP_IN_EP, dap_tx, result & 0xFFFFU) != USBD_OK) {
      dap_tx_busy = 0U;
    }
    (void)USBD_LL_PrepareReceive(&usb_device, RB_DAP_OUT_EP, dap_rx, sizeof(dap_rx));
  }
  if (!cdc_tx_busy && (usb_device.dev_state == USBD_STATE_CONFIGURED)) {
    uint16_t count = RB_UART_Read(cdc_tx, sizeof(cdc_tx));
    if (count != 0U) {
      cdc_tx_busy = 1U;
      if (USBD_LL_Transmit(&usb_device, RB_CDC_IN_EP, cdc_tx, count) != USBD_OK) cdc_tx_busy = 0U;
    } else {
      RB_LED_Data(0U);
    }
  }
}

/* ---------------- Device descriptors ---------------- */
__ALIGN_BEGIN static uint8_t device_desc[] __ALIGN_END = {
  0x12, USB_DESC_TYPE_DEVICE, 0x10,0x02, 0xEF,0x02,0x01, 0x40,
  0x51,0xC2, 0x01,0xF0, 0x00,0x03, 0x01,0x02,0x03,0x01
};
__ALIGN_BEGIN static uint8_t lang_desc[] __ALIGN_END = {0x04, USB_DESC_TYPE_STRING, 0x09,0x04};
__ALIGN_BEGIN static uint8_t string_desc[128] __ALIGN_END;

static uint8_t *dev_desc(USBD_SpeedTypeDef speed, uint16_t *len) { (void)speed; *len=sizeof(device_desc); return device_desc; }
static uint8_t *lang(USBD_SpeedTypeDef speed, uint16_t *len) { (void)speed; *len=sizeof(lang_desc); return lang_desc; }
static uint8_t *make_string(const char *s, uint16_t *len) { USBD_GetString((uint8_t *)s, string_desc, len); return string_desc; }
static uint8_t *manufacturer(USBD_SpeedTypeDef speed, uint16_t *len) { (void)speed; return make_string("RB Technology",len); }
static uint8_t *product(USBD_SpeedTypeDef speed, uint16_t *len) { (void)speed; return make_string("RBLink V3.0",len); }
static uint8_t *serial(USBD_SpeedTypeDef speed, uint16_t *len)
{
  static char text[25]; static const char hex[]="0123456789ABCDEF"; uint32_t i; const uint8_t *uid=(const uint8_t *)UID_BASE;
  (void)speed; for(i=0U;i<12U;i++){text[2U*i]=hex[uid[i]>>4U];text[2U*i+1U]=hex[uid[i]&15U];} text[24]='\0'; return make_string(text,len);
}
static uint8_t *configuration(USBD_SpeedTypeDef speed, uint16_t *len) { (void)speed; return make_string("RBLink Composite",len); }
static uint8_t *interface_name(USBD_SpeedTypeDef speed, uint16_t *len) { (void)speed; return make_string("RBLink CMSIS-DAP",len); }
static uint8_t *bos(USBD_SpeedTypeDef speed, uint16_t *len) { (void)speed; *len=sizeof(bos_desc); return bos_desc; }

static USBD_DescriptorsTypeDef descriptors = {
  dev_desc, lang, manufacturer, product, serial, configuration, interface_name, bos
};

void RB_USB_Init(void)
{
  DAP_Setup();
  RB_UART_Init();
  (void)USBD_Init(&usb_device, &descriptors, 0U);
  (void)USBD_RegisterClass(&usb_device, &rblink_class);
  (void)USBD_Start(&usb_device);
}

/* ---------------- STM32 HAL PCD low-level glue ---------------- */
void *USBD_static_malloc(uint32_t size) { static uint32_t memory[32]; (void)size; return memory; }
void USBD_static_free(void *p) { (void)p; }

USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev)
{
  hpcd_USB_OTG_FS.Instance = USB_OTG_FS;
  hpcd_USB_OTG_FS.Init.dev_endpoints = 4U;
  hpcd_USB_OTG_FS.Init.speed = PCD_SPEED_FULL;
  hpcd_USB_OTG_FS.Init.dma_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.phy_itface = PCD_PHY_EMBEDDED;
  hpcd_USB_OTG_FS.Init.Sof_enable = ENABLE;
  hpcd_USB_OTG_FS.Init.low_power_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.lpm_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.vbus_sensing_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.use_dedicated_ep1 = DISABLE;
  hpcd_USB_OTG_FS.pData = pdev; pdev->pData = &hpcd_USB_OTG_FS;
  if (HAL_PCD_Init(&hpcd_USB_OTG_FS) != HAL_OK) return USBD_FAIL;
  (void)HAL_PCDEx_SetRxFiFo(&hpcd_USB_OTG_FS, 128U);
  (void)HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_FS, 0U, 32U);
  (void)HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_FS, 1U, 64U);
  (void)HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_FS, 2U, 16U);
  (void)HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_FS, 3U, 64U);
  HAL_NVIC_SetPriority(OTG_FS_IRQn, 5U, 0U); HAL_NVIC_EnableIRQ(OTG_FS_IRQn);
  return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *p){return HAL_PCD_DeInit((PCD_HandleTypeDef*)p->pData)==HAL_OK?USBD_OK:USBD_FAIL;}
USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *p){return HAL_PCD_Start((PCD_HandleTypeDef*)p->pData)==HAL_OK?USBD_OK:USBD_FAIL;}
USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *p){return HAL_PCD_Stop((PCD_HandleTypeDef*)p->pData)==HAL_OK?USBD_OK:USBD_FAIL;}
USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *p,uint8_t a,uint8_t t,uint16_t m){return HAL_PCD_EP_Open((PCD_HandleTypeDef*)p->pData,a,m,t)==HAL_OK?USBD_OK:USBD_FAIL;}
USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *p,uint8_t a){return HAL_PCD_EP_Close((PCD_HandleTypeDef*)p->pData,a)==HAL_OK?USBD_OK:USBD_FAIL;}
USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *p,uint8_t a){return HAL_PCD_EP_Flush((PCD_HandleTypeDef*)p->pData,a)==HAL_OK?USBD_OK:USBD_FAIL;}
USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *p,uint8_t a){return HAL_PCD_EP_SetStall((PCD_HandleTypeDef*)p->pData,a)==HAL_OK?USBD_OK:USBD_FAIL;}
USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *p,uint8_t a){return HAL_PCD_EP_ClrStall((PCD_HandleTypeDef*)p->pData,a)==HAL_OK?USBD_OK:USBD_FAIL;}
uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *p,uint8_t a){PCD_HandleTypeDef*h=p->pData;return (a&0x80U)?h->IN_ep[a&0x7FU].is_stall:h->OUT_ep[a&0x7FU].is_stall;}
USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *p,uint8_t a){return HAL_PCD_SetAddress((PCD_HandleTypeDef*)p->pData,a)==HAL_OK?USBD_OK:USBD_FAIL;}
USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *p,uint8_t a,uint8_t*b,uint32_t n){return HAL_PCD_EP_Transmit((PCD_HandleTypeDef*)p->pData,a,b,n)==HAL_OK?USBD_OK:USBD_FAIL;}
USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *p,uint8_t a,uint8_t*b,uint32_t n){return HAL_PCD_EP_Receive((PCD_HandleTypeDef*)p->pData,a,b,n)==HAL_OK?USBD_OK:USBD_FAIL;}
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *p,uint8_t a){return HAL_PCD_EP_GetRxCount((PCD_HandleTypeDef*)p->pData,a);}
void USBD_LL_Delay(uint32_t d){HAL_Delay(d);}

void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *h){(void)USBD_LL_SetupStage(h->pData,(uint8_t*)h->Setup);}
void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *h,uint8_t ep){(void)USBD_LL_DataOutStage(h->pData,ep,h->OUT_ep[ep].xfer_buff);}
void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *h,uint8_t ep){(void)USBD_LL_DataInStage(h->pData,ep,h->IN_ep[ep].xfer_buff);}
void HAL_PCD_SOFCallback(PCD_HandleTypeDef *h){(void)USBD_LL_SOF(h->pData);}
void HAL_PCD_ResetCallback(PCD_HandleTypeDef *h){(void)USBD_LL_SetSpeed(h->pData,USBD_SPEED_FULL);(void)USBD_LL_Reset(h->pData);}
void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *h){(void)USBD_LL_Suspend(h->pData);}
void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *h){(void)USBD_LL_Resume(h->pData);}
void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *h){(void)USBD_LL_DevConnected(h->pData);}
void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *h){(void)USBD_LL_DevDisconnected(h->pData);}
