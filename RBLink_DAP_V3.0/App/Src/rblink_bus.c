/* RBLink V3 CMSIS-DAP vendor-command hardware backend. */
#include <string.h>
#include "stm32f4xx_hal.h"
#include "rblink_board.h"

#define LT_OK          0x00U
#define LT_BAD_LENGTH  0x01U
#define LT_BAD_ACTION  0x02U
#define LT_IO_ERROR    0x03U
#define LT_TIMEOUT_MS  100U

static SPI_HandleTypeDef hspi2;
static I2C_HandleTypeDef hi2c3;
static CAN_HandleTypeDef hcan1;
static ADC_HandleTypeDef hadc1;
static DAC_HandleTypeDef hdac;
static uint8_t power_initialized;
static uint8_t power_enabled;

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8U);
}

static uint8_t spi_config(const uint8_t *p, uint8_t len)
{
    GPIO_InitTypeDef gpio = {0};
    uint32_t speed;
    uint32_t divider;

    /* [CONFIG][speed_Hz:u32][mode:0..3][lsb_first:0/1] */
    if (len != 7U) return LT_BAD_LENGTH;
    speed = get_u32(&p[1]);
    if ((speed == 0U) || (p[5] > 3U) || (p[6] > 1U)) return LT_BAD_LENGTH;

    __HAL_RCC_GPIOB_CLK_ENABLE();
    /* Always release chip select before reconfiguring clock polarity. */
    HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN, GPIO_PIN_SET);
    __HAL_RCC_SPI2_CLK_ENABLE();
    gpio.Pin = RB_SPI_PINS;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* SPI2 kernel clock is APB1=42 MHz. Choose the fastest divider not above
     * the requested clock. */
    divider = SPI_BAUDRATEPRESCALER_256;
    if (speed >= 21000000U) divider = SPI_BAUDRATEPRESCALER_2;
    else if (speed >= 10500000U) divider = SPI_BAUDRATEPRESCALER_4;
    else if (speed >= 5250000U) divider = SPI_BAUDRATEPRESCALER_8;
    else if (speed >= 2625000U) divider = SPI_BAUDRATEPRESCALER_16;
    else if (speed >= 1312500U) divider = SPI_BAUDRATEPRESCALER_32;
    else if (speed >= 656250U) divider = SPI_BAUDRATEPRESCALER_64;
    else if (speed >= 328125U) divider = SPI_BAUDRATEPRESCALER_128;

    memset(&hspi2, 0, sizeof(hspi2));
    hspi2.Instance = SPI2;
    hspi2.Init.Mode = SPI_MODE_MASTER;
    hspi2.Init.Direction = SPI_DIRECTION_2LINES;
    hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi2.Init.CLKPolarity = (p[5] & 2U) ? SPI_POLARITY_HIGH : SPI_POLARITY_LOW;
    hspi2.Init.CLKPhase = (p[5] & 1U) ? SPI_PHASE_2EDGE : SPI_PHASE_1EDGE;
    hspi2.Init.NSS = SPI_NSS_SOFT;
    hspi2.Init.BaudRatePrescaler = divider;
    hspi2.Init.FirstBit = p[6] ? SPI_FIRSTBIT_LSB : SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi2.Init.CRCPolynomial = 7U;
    if (HAL_SPI_Init(&hspi2) != HAL_OK) return LT_IO_ERROR;
    HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN, GPIO_PIN_SET);
    RB_SPI_Enable(1U);
    return LT_OK;
}

uint8_t rblink_platform_spi(const uint8_t *p, uint8_t len, uint8_t *r, uint8_t *rlen)
{
    uint8_t count;
    uint8_t tx[56] = {0xFFU};
    uint8_t rx[56];
    if (p[0] == 2U) {
        if (len != 1U) return LT_BAD_LENGTH;
        HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN, GPIO_PIN_SET);
        RB_SPI_Enable(0U);
        if (hspi2.Instance == SPI2) (void)HAL_SPI_DeInit(&hspi2);
        memset(&hspi2, 0, sizeof(hspi2));
        return LT_OK;
    }
    if (p[0] == 0U) return spi_config(p, len);
    /* [TRANSFER][cs_hold][count][data...] */
    if ((len < 3U) || (p[1] > 1U) || (p[2] > 53U) ||
        (len != (uint8_t)(3U + p[2])) ||
        (hspi2.Instance != SPI2)) return LT_BAD_LENGTH;
    count = p[2];
    /* A zero-byte transfer is an explicit CS control/recovery operation. */
    if (count == 0U) {
        HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN,
                         p[1] ? GPIO_PIN_RESET : GPIO_PIN_SET);
        *rlen = 0U;
        return LT_OK;
    }
    memcpy(tx, &p[3], count);
    HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN, GPIO_PIN_RESET);
    if (HAL_SPI_TransmitReceive(&hspi2, tx, rx, count, LT_TIMEOUT_MS) != HAL_OK) {
        HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN, GPIO_PIN_SET);
        return LT_IO_ERROR;
    }
    if (p[1] == 0U) HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN, GPIO_PIN_SET);
    memcpy(r, rx, count);
    *rlen = count;
    return LT_OK;
}

static uint8_t i2c_config(const uint8_t *p, uint8_t len)
{
    GPIO_InitTypeDef gpio = {0};
    if (len != 5U) return LT_BAD_LENGTH;
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_I2C3_CLK_ENABLE();
    gpio.Pin = RB_I2C_SCL_PIN;
    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Pull = GPIO_NOPULL; /* External 4.7 kOhm pull-ups are fitted. */
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF4_I2C3;
    HAL_GPIO_Init(RB_I2C_SCL_PORT, &gpio);
    gpio.Pin = RB_I2C_SDA_PIN;
    HAL_GPIO_Init(RB_I2C_SDA_PORT, &gpio);
    memset(&hi2c3, 0, sizeof(hi2c3));
    hi2c3.Instance = I2C3;
    hi2c3.Init.ClockSpeed = get_u32(&p[1]);
    if ((hi2c3.Init.ClockSpeed == 0U) || (hi2c3.Init.ClockSpeed > 400000U)) return LT_BAD_LENGTH;
    hi2c3.Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c3.Init.OwnAddress1 = 0U;
    hi2c3.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c3.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c3.Init.OwnAddress2 = 0U;
    hi2c3.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c3.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c3) != HAL_OK) return LT_IO_ERROR;
    RB_I2C_Enable(1U);
    return LT_OK;
}

static void i2c_recover_bus(void)
{
    GPIO_InitTypeDef gpio = {0};
    uint32_t pulse;

    /* A target reset in the middle of a byte can leave SDA low. Temporarily
     * become GPIO open-drain, provide up to nine clocks, generate STOP, then
     * restore I2C3. Never drive either line high. */
    (void)HAL_I2C_DeInit(&hi2c3);
    gpio.Pin = RB_I2C_SCL_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(RB_I2C_SCL_PORT, &gpio);
    gpio.Pin = RB_I2C_SDA_PIN;
    HAL_GPIO_Init(RB_I2C_SDA_PORT, &gpio);
    HAL_GPIO_WritePin(RB_I2C_SCL_PORT, RB_I2C_SCL_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(RB_I2C_SDA_PORT, RB_I2C_SDA_PIN, GPIO_PIN_SET);
    for (pulse = 0U; (pulse < 9U) &&
         (HAL_GPIO_ReadPin(RB_I2C_SDA_PORT, RB_I2C_SDA_PIN) == GPIO_PIN_RESET); ++pulse) {
        HAL_GPIO_WritePin(RB_I2C_SCL_PORT, RB_I2C_SCL_PIN, GPIO_PIN_RESET);
        HAL_Delay(1U);
        HAL_GPIO_WritePin(RB_I2C_SCL_PORT, RB_I2C_SCL_PIN, GPIO_PIN_SET);
        HAL_Delay(1U);
    }
    HAL_GPIO_WritePin(RB_I2C_SDA_PORT, RB_I2C_SDA_PIN, GPIO_PIN_RESET);
    HAL_Delay(1U);
    HAL_GPIO_WritePin(RB_I2C_SCL_PORT, RB_I2C_SCL_PIN, GPIO_PIN_SET);
    HAL_Delay(1U);
    HAL_GPIO_WritePin(RB_I2C_SDA_PORT, RB_I2C_SDA_PIN, GPIO_PIN_SET);

    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF4_I2C3;
    gpio.Pin = RB_I2C_SCL_PIN;
    HAL_GPIO_Init(RB_I2C_SCL_PORT, &gpio);
    gpio.Pin = RB_I2C_SDA_PIN;
    HAL_GPIO_Init(RB_I2C_SDA_PORT, &gpio);
    (void)HAL_I2C_Init(&hi2c3);
}

static uint8_t i2c_failed(void)
{
    /* NACK is a normal device-level error; recover only bus/controller faults. */
    if (HAL_I2C_GetError(&hi2c3) != HAL_I2C_ERROR_AF) {
        i2c_recover_bus();
    }
    return LT_IO_ERROR;
}

uint8_t rblink_platform_i2c(const uint8_t *p, uint8_t len, uint8_t *r, uint8_t *rlen)
{
    uint8_t addr, txlen, rxlen;
    if (p[0] == 2U) {
        if (len != 1U) return LT_BAD_LENGTH;
        RB_I2C_Enable(0U);
        if (hi2c3.Instance == I2C3) (void)HAL_I2C_DeInit(&hi2c3);
        memset(&hi2c3, 0, sizeof(hi2c3));
        return LT_OK;
    }
    if (p[0] == 0U) return i2c_config(p, len);
    /* [TRANSFER][7-bit address][write_len][read_len][write_data...] */
    if ((len < 4U) || (hi2c3.Instance != I2C3)) return LT_BAD_LENGTH;
    addr = p[1]; txlen = p[2]; rxlen = p[3];
    if ((addr > 0x7FU) || (rxlen > 56U) || (len != (uint8_t)(4U + txlen))) return LT_BAD_LENGTH;
    /* One- and two-byte register addresses use HAL memory transactions, which
     * generate the repeated START expected by most sensor/register devices. */
    if ((rxlen != 0U) && ((txlen == 1U) || (txlen == 2U))) {
        uint16_t reg = (txlen == 1U) ? p[4] : ((uint16_t)p[4] << 8U) | p[5];
        uint16_t reg_size = (txlen == 1U) ? I2C_MEMADD_SIZE_8BIT : I2C_MEMADD_SIZE_16BIT;
        if (HAL_I2C_Mem_Read(&hi2c3, (uint16_t)addr << 1U, reg, reg_size,
                             r, rxlen, LT_TIMEOUT_MS) != HAL_OK) return i2c_failed();
        *rlen = rxlen;
        return LT_OK;
    }
    if ((txlen != 0U) && (HAL_I2C_Master_Transmit(&hi2c3, (uint16_t)addr << 1U,
                                                  (uint8_t *)&p[4], txlen, LT_TIMEOUT_MS) != HAL_OK))
        return i2c_failed();
    if ((rxlen != 0U) && (HAL_I2C_Master_Receive(&hi2c3, (uint16_t)addr << 1U,
                                                 r, rxlen, LT_TIMEOUT_MS) != HAL_OK))
        return i2c_failed();
    *rlen = rxlen;
    return LT_OK;
}

uint8_t rblink_platform_can(const uint8_t *p, uint8_t len, uint8_t *r, uint8_t *rlen)
{
    CAN_TxHeaderTypeDef header = {0};
    CAN_RxHeaderTypeDef rx_header = {0};
    CAN_FilterTypeDef filter = {0};
    uint32_t mailbox;
    uint32_t bitrate, prescaler;
    GPIO_InitTypeDef gpio = {0};
    if (p[0] == 2U) {
        if (len != 1U) return LT_BAD_LENGTH;
        if (hcan1.Instance == CAN1) {
            (void)HAL_CAN_Stop(&hcan1);
            (void)HAL_CAN_DeInit(&hcan1);
        }
        memset(&hcan1, 0, sizeof(hcan1));
        return LT_OK;
    }
    if (p[0] == 0U) {
        /* [CONFIG][bitrate:u32], 14 time quanta/bit, 85.7% sample point. */
        if (len != 5U) return LT_BAD_LENGTH;
        bitrate = get_u32(&p[1]);
        if ((bitrate == 0U) || (bitrate > 1000000U) ||
            ((42000000U % (bitrate * 14U)) != 0U)) return LT_BAD_LENGTH;
        prescaler = 42000000U / (bitrate * 14U);
        if ((prescaler == 0U) || (prescaler > 1024U)) return LT_BAD_LENGTH;
        __HAL_RCC_GPIOB_CLK_ENABLE(); __HAL_RCC_CAN1_CLK_ENABLE();
        gpio.Pin = RB_CAN_PINS;
        gpio.Mode = GPIO_MODE_AF_PP; gpio.Pull = GPIO_NOPULL;
        gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH; gpio.Alternate = GPIO_AF9_CAN1;
        HAL_GPIO_Init(GPIOB, &gpio);
        memset(&hcan1, 0, sizeof(hcan1));
        hcan1.Instance = CAN1; hcan1.Init.Prescaler = prescaler;
        hcan1.Init.Mode = CAN_MODE_NORMAL; hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
        hcan1.Init.TimeSeg1 = CAN_BS1_11TQ; hcan1.Init.TimeSeg2 = CAN_BS2_2TQ;
        hcan1.Init.TimeTriggeredMode = DISABLE; hcan1.Init.AutoBusOff = ENABLE;
        hcan1.Init.AutoWakeUp = DISABLE; hcan1.Init.AutoRetransmission = ENABLE;
        hcan1.Init.ReceiveFifoLocked = DISABLE; hcan1.Init.TransmitFifoPriority = DISABLE;
        if (HAL_CAN_Init(&hcan1) != HAL_OK) return LT_IO_ERROR;
        filter.FilterBank = 0U; filter.FilterMode = CAN_FILTERMODE_IDMASK;
        filter.FilterScale = CAN_FILTERSCALE_32BIT; filter.FilterIdHigh = 0U;
        filter.FilterIdLow = 0U; filter.FilterMaskIdHigh = 0U; filter.FilterMaskIdLow = 0U;
        filter.FilterFIFOAssignment = CAN_RX_FIFO0; filter.FilterActivation = ENABLE;
        filter.SlaveStartFilterBank = 14U;
        if ((HAL_CAN_ConfigFilter(&hcan1, &filter) != HAL_OK) ||
            (HAL_CAN_Start(&hcan1) != HAL_OK)) return LT_IO_ERROR;
        return LT_OK;
    }
    /* [TRANSFER][flags][id:u32][dlc][data...]. flags: bit0=extended,
     * bit1=remote, bit7=receive-one-frame instead of transmit. */
    if ((len < 7U) || (hcan1.Instance != CAN1)) return LT_BAD_LENGTH;
    if ((p[1] & 0x80U) != 0U) {
        uint32_t id;
        if ((len != 7U) || (p[6] != 0U) || ((p[1] & 0x7CU) != 0U)) return LT_BAD_LENGTH;
        if (HAL_CAN_GetRxFifoFillLevel(&hcan1, CAN_RX_FIFO0) == 0U) {
            *rlen = 0U;
            return LT_OK;
        }
        if (HAL_CAN_GetRxMessage(&hcan1, CAN_RX_FIFO0, &rx_header, &r[6]) != HAL_OK) return LT_IO_ERROR;
        id = (rx_header.IDE == CAN_ID_EXT) ? rx_header.ExtId : rx_header.StdId;
        r[0] = (rx_header.IDE == CAN_ID_EXT ? 1U : 0U) |
               (rx_header.RTR == CAN_RTR_REMOTE ? 2U : 0U);
        r[1] = (uint8_t)id; r[2] = (uint8_t)(id >> 8U);
        r[3] = (uint8_t)(id >> 16U); r[4] = (uint8_t)(id >> 24U);
        r[5] = (uint8_t)rx_header.DLC; *rlen = (uint8_t)(6U + rx_header.DLC);
        return LT_OK;
    }
    if (((p[1] & 0xFCU) != 0U) || (p[6] > 8U) ||
        (len != (uint8_t)(7U + p[6]))) return LT_BAD_LENGTH;
    if ((p[1] & 1U) != 0U) {
        if (get_u32(&p[2]) > 0x1FFFFFFFU) return LT_BAD_LENGTH;
        header.ExtId = get_u32(&p[2]); header.IDE = CAN_ID_EXT;
    } else {
        if (get_u32(&p[2]) > 0x7FFU) return LT_BAD_LENGTH;
        header.StdId = get_u32(&p[2]); header.IDE = CAN_ID_STD;
    }
    header.RTR = ((p[1] & 2U) != 0U) ? CAN_RTR_REMOTE : CAN_RTR_DATA;
    header.DLC = p[6]; *rlen = 0U;
    return (HAL_CAN_AddTxMessage(&hcan1, &header, (uint8_t *)&p[7], &mailbox) == HAL_OK) ? LT_OK : LT_IO_ERROR;
}

static uint16_t adc_read(uint32_t channel)
{
    ADC_ChannelConfTypeDef cfg = {0};
    cfg.Channel = channel; cfg.Rank = 1U; cfg.SamplingTime = ADC_SAMPLETIME_144CYCLES;
    if ((HAL_ADC_ConfigChannel(&hadc1, &cfg) != HAL_OK) ||
        (HAL_ADC_Start(&hadc1) != HAL_OK) ||
        (HAL_ADC_PollForConversion(&hadc1, 10U) != HAL_OK)) return 0xFFFFU;
    return (uint16_t)HAL_ADC_GetValue(&hadc1);
}

static void power_off(void)
{
    /* Remove the load-facing rail first, then its upstream boost supply. */
    HAL_GPIO_WritePin(RB_LDO_EN_PORT, RB_LDO_EN_PIN, GPIO_PIN_RESET);
    HAL_Delay(1U);
    HAL_GPIO_WritePin(RB_BOOST_EN_PORT, RB_BOOST_EN_PIN, GPIO_PIN_RESET);
    power_enabled = 0U;
}

uint8_t rblink_platform_power(const uint8_t *p, uint8_t len, uint8_t *r, uint8_t *rlen)
{
    GPIO_InitTypeDef gpio = {0};
    DAC_ChannelConfTypeDef dcfg = {0};
    uint16_t dac_code;
    if (p[0] == 2U) {
        if (len != 1U) return LT_BAD_LENGTH;
        power_off();
        return LT_OK;
    }
    if (p[0] == 0U) {
        /* [CONFIG][enable][raw DAC code:u16]. Raw code is intentional until
         * the final FB injection network is measured and calibrated. */
        if ((len != 4U) || (p[1] > 1U)) return LT_BAD_LENGTH;
        dac_code = (uint16_t)p[2] | ((uint16_t)p[3] << 8U);
        if (dac_code > 4095U) return LT_BAD_LENGTH;
        __HAL_RCC_GPIOA_CLK_ENABLE(); __HAL_RCC_ADC1_CLK_ENABLE(); __HAL_RCC_DAC_CLK_ENABLE();
        gpio.Pin = RB_ADC_VREF_PIN | RB_ADC_VOUT_PIN | RB_ADC_NTC_PIN | RB_DAC_VOUT_PIN;
        gpio.Mode = GPIO_MODE_ANALOG; gpio.Pull = GPIO_NOPULL; HAL_GPIO_Init(GPIOA, &gpio);
        hadc1.Instance = ADC1; hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
        hadc1.Init.Resolution = ADC_RESOLUTION_12B; hadc1.Init.ScanConvMode = DISABLE;
        hadc1.Init.ContinuousConvMode = DISABLE; hadc1.Init.DiscontinuousConvMode = DISABLE;
        hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
        hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START; hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
        hadc1.Init.NbrOfConversion = 1U; hadc1.Init.DMAContinuousRequests = DISABLE;
        hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
        hdac.Instance = DAC;
        if ((HAL_ADC_Init(&hadc1) != HAL_OK) || (HAL_DAC_Init(&hdac) != HAL_OK)) {
            power_off();
            return LT_IO_ERROR;
        }
        dcfg.DAC_Trigger = DAC_TRIGGER_NONE; dcfg.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
        if ((HAL_DAC_ConfigChannel(&hdac, &dcfg, DAC_CHANNEL_1) != HAL_OK) ||
            (HAL_DAC_SetValue(&hdac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, dac_code) != HAL_OK) ||
            (HAL_DAC_Start(&hdac, DAC_CHANNEL_1) != HAL_OK)) {
            power_off();
            return LT_IO_ERROR;
        }
        power_initialized = 1U;
        if (p[1]) { HAL_GPIO_WritePin(RB_BOOST_EN_PORT, RB_BOOST_EN_PIN, GPIO_PIN_SET);
                    HAL_Delay(5U); HAL_GPIO_WritePin(RB_LDO_EN_PORT, RB_LDO_EN_PIN, GPIO_PIN_SET);
                    power_enabled = 1U; }
        else { power_off(); }
        return LT_OK;
    }
    if (len != 1U) return LT_BAD_LENGTH;
    if (!power_initialized) return LT_IO_ERROR;
    r[0] = power_enabled;
    /* Keep the wire response order VREF, VOUT, NTC despite the PCB pin move. */
    put_u16(&r[1], adc_read(ADC_CHANNEL_2));
    put_u16(&r[3], adc_read(ADC_CHANNEL_1));
    put_u16(&r[5], adc_read(ADC_CHANNEL_0));
    if ((r[1] == 0xFFU && r[2] == 0xFFU) ||
        (r[3] == 0xFFU && r[4] == 0xFFU) ||
        (r[5] == 0xFFU && r[6] == 0xFFU)) return LT_IO_ERROR;
    *rlen = 7U;
    return LT_OK;
}
