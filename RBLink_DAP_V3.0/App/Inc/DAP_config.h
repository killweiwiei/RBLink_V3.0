#ifndef RBLINK_DAP_CONFIG_H
#define RBLINK_DAP_CONFIG_H

#include <string.h>
#include "rblink_board.h"

#define CPU_CLOCK                    168000000U
#define IO_PORT_WRITE_CYCLES         2U
#define DAP_SWD                      1
#define DAP_JTAG                     1
#define DAP_JTAG_DEV_CNT             8U
#define DAP_DEFAULT_PORT             1U
#define DAP_DEFAULT_SWJ_CLOCK        5000000U
#define DAP_PACKET_SIZE              64U
#define DAP_PACKET_COUNT             1U
#define SWO_UART                     0
#define SWO_UART_DRIVER              0
#define SWO_UART_MAX_BAUDRATE        10000000U
#define SWO_MANCHESTER               0
#define SWO_BUFFER_SIZE              4096U
#define SWO_STREAM                   0
#define TIMESTAMP_CLOCK              1000000U
#define DAP_UART                     0
#define DAP_UART_DRIVER              0
#define DAP_UART_RX_BUFFER_SIZE      512U
#define DAP_UART_TX_BUFFER_SIZE      512U
#define DAP_UART_USB_COM_PORT        1
#define TARGET_FIXED                 0

static __INLINE uint8_t rb_copy_string(char *dst, const char *src)
{
  uint8_t n = (uint8_t)(strlen(src) + 1U);
  memcpy(dst, src, n);
  return n;
}

__STATIC_INLINE uint8_t DAP_GetVendorString(char *str) { return rb_copy_string(str, "RB Technology"); }
__STATIC_INLINE uint8_t DAP_GetProductString(char *str) { return rb_copy_string(str, "RBLink CMSIS-DAP"); }
__STATIC_INLINE uint8_t DAP_GetSerNumString(char *str)
{
  static const char hex[] = "0123456789ABCDEF";
  const uint32_t *uid = (const uint32_t *)UID_BASE;
  uint32_t i;
  for (i = 0U; i < 12U; ++i) {
    uint8_t b = ((const uint8_t *)uid)[i];
    str[i * 2U] = hex[b >> 4U];
    str[i * 2U + 1U] = hex[b & 0x0FU];
  }
  str[24] = '\0';
  return 25U;
}
__STATIC_INLINE uint8_t DAP_GetTargetDeviceVendorString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetDeviceNameString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetBoardVendorString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetBoardNameString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetProductFirmwareVersionString(char *str) { return rb_copy_string(str, "V3.0.05"); }

__STATIC_FORCEINLINE void rb_set(GPIO_TypeDef *p, uint32_t n) { p->BSRR = n; }
__STATIC_FORCEINLINE void rb_clr(GPIO_TypeDef *p, uint32_t n) { p->BSRR = n << 16U; }
__STATIC_FORCEINLINE uint32_t rb_read(GPIO_TypeDef *p, uint32_t n) { return (p->IDR & n) != 0U; }

/* PD13 and PD12 are the drive and sense paths of the same target SWDIO net.
   Merely changing the SN74LVC1T45 direction is insufficient: PD13 must be
   high-impedance while the target owns the bus, otherwise it can contend
   with the translator output and force the sampled ACK high. */
__STATIC_FORCEINLINE void rb_swdio_drive_release(void)
{
  RB_SWDIO_OUT_PORT->MODER &= ~GPIO_MODER_MODER13_Msk;
  __DSB();
  rb_clr(RB_SWDIO_DIR_PORT, RB_SWDIO_DIR_PIN);
  __DSB();
}

__STATIC_FORCEINLINE void rb_swdio_drive_enable(void)
{
  /* Keep the line parked high while ownership changes back to the probe. */
  rb_set(RB_SWDIO_OUT_PORT, RB_SWDIO_OUT_PIN);
  rb_set(RB_SWDIO_DIR_PORT, RB_SWDIO_DIR_PIN);
  __DSB();
  RB_SWDIO_OUT_PORT->MODER =
      (RB_SWDIO_OUT_PORT->MODER & ~GPIO_MODER_MODER13_Msk) |
      (1UL << GPIO_MODER_MODER13_Pos);
  __DSB();
}

__STATIC_INLINE void PORT_JTAG_SETUP(void) { rb_swdio_drive_enable(); RB_DAP_Enable(1U); }
__STATIC_INLINE void PORT_SWD_SETUP(void)  { rb_swdio_drive_enable(); RB_DAP_Enable(1U); }
__STATIC_INLINE void PORT_OFF(void)
{
  /* Leave the JTAG/SWD translator enabled as required by the RBLink V3
     hardware contract. Only release the bidirectional SWDIO driver here. */
  RB_DAP_Enable(1U);
  rb_swdio_drive_release();
  rb_clr(RB_NRESET_PORT, RB_NRESET_PIN);
}

__STATIC_FORCEINLINE uint32_t PIN_SWCLK_TCK_IN(void) { return (RB_SWCLK_PORT->ODR & RB_SWCLK_PIN) != 0U; }
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_SET(void) { rb_set(RB_SWCLK_PORT, RB_SWCLK_PIN); }
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_CLR(void) { rb_clr(RB_SWCLK_PORT, RB_SWCLK_PIN); }
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_TMS_IN(void) { return rb_read(RB_SWDIO_IN_PORT, RB_SWDIO_IN_PIN); }
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_SET(void) { rb_set(RB_SWDIO_OUT_PORT, RB_SWDIO_OUT_PIN); }
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_CLR(void) { rb_clr(RB_SWDIO_OUT_PORT, RB_SWDIO_OUT_PIN); }
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_IN(void) { return rb_read(RB_SWDIO_IN_PORT, RB_SWDIO_IN_PIN); }
/* CMSIS-DAP passes right-shifted words to the bit output hooks.  Only bit 0
   is the current wire bit; testing the whole value changes a valid SWD A5
   request into A7 whenever any higher request bit remains set. */
__STATIC_FORCEINLINE void PIN_SWDIO_OUT(uint32_t bit) { (bit & 1U) ? PIN_SWDIO_TMS_SET() : PIN_SWDIO_TMS_CLR(); }
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_ENABLE(void) { rb_swdio_drive_enable(); }
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_DISABLE(void) { rb_swdio_drive_release(); }
__STATIC_FORCEINLINE uint32_t PIN_TDI_IN(void) { return (RB_TDI_PORT->ODR & RB_TDI_PIN) != 0U; }
__STATIC_FORCEINLINE void PIN_TDI_OUT(uint32_t bit) { (bit & 1U) ? rb_set(RB_TDI_PORT, RB_TDI_PIN) : rb_clr(RB_TDI_PORT, RB_TDI_PIN); }
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void) { return rb_read(RB_TDO_PORT, RB_TDO_PIN); }
__STATIC_FORCEINLINE uint32_t PIN_nTRST_IN(void) { return (RB_NTRST_PORT->ODR & RB_NTRST_PIN) != 0U; }
__STATIC_FORCEINLINE void PIN_nTRST_OUT(uint32_t bit) { (bit & 1U) ? rb_set(RB_NTRST_PORT, RB_NTRST_PIN) : rb_clr(RB_NTRST_PORT, RB_NTRST_PIN); }
__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN(void) { return (RB_NRESET_PORT->ODR & RB_NRESET_PIN) == 0U; }
__STATIC_FORCEINLINE void PIN_nRESET_OUT(uint32_t bit) { (bit & 1U) ? rb_clr(RB_NRESET_PORT, RB_NRESET_PIN) : rb_set(RB_NRESET_PORT, RB_NRESET_PIN); }
__STATIC_INLINE void LED_CONNECTED_OUT(uint32_t bit) { RB_LED_State((uint8_t)bit); }
__STATIC_INLINE void LED_RUNNING_OUT(uint32_t bit) { RB_LED_Data((uint8_t)bit); }
__STATIC_INLINE uint32_t TIMESTAMP_GET(void) { return DWT->CYCCNT / 168U; }
__STATIC_INLINE void DAP_SETUP(void)
{
  /* Board GPIO ownership is established once by main(). Reinitializing it
     here would generate a second ESP CHIP_EN reset pulse during USB startup. */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
__STATIC_INLINE uint32_t RESET_TARGET(void) { return 0U; }

#endif
