/* RBLink V3 CMSIS-DAP vendor-command hardware backend. */
#include <string.h>
#include "stm32f4xx_hal.h"
#include "rblink_board.h"
#include "rblink_bus.h"

#define LT_OK          0x00U
#define LT_BAD_LENGTH  0x01U
#define LT_BAD_ACTION  0x02U
#define LT_IO_ERROR    0x03U
#define LT_BUSY        0x04U
#define LT_TIMEOUT_MS  100U

static SPI_HandleTypeDef hspi2;
static I2C_HandleTypeDef hi2c3;
#define RB_I2C_RX_SIZE 1024U
#define RB_I2C_TX_SIZE 48U
/* Single-producer/consumer ring; shared indices and counters are protected
 * briefly while constructing a USB response. No USB operation runs in an IRQ. */
static struct {
    volatile uint8_t enabled, direction, active;
    uint8_t address, filler, tx[RB_I2C_TX_SIZE], tx_length, tx_index;
    uint8_t rx[RB_I2C_RX_SIZE];
    volatile uint16_t head, tail, writes, reads;
    volatile uint32_t received, supplied, dropped, errors;
} i2c_slave;

/* Upload into the inactive bank; only commit exposes a complete register map. */
static struct {
    uint8_t bank[2][256];
    volatile uint8_t enabled, current, pointer, expect_address;
    uint8_t staging, pending, address, filler;
} i2c_map;

#define TCU_ERR_LENGTH   0x01U
#define TCU_ERR_DATA     0x02U
#define TCU_ERR_BUS      0x04U
#define TCU_ERR_SEQUENCE 0x08U
#define TCU_ERR_TIMEOUT  0x10U
#define TCU_ERR_GAP      0x20U
#define TCU_ERR_IDLE     0x40U
/* Application-owned one-shot test policy, separate from the generic bus API.
 * State: 1=first write, 2=gap/await read, 3=done, 4=failed, 5=await STOP. */
static struct {
    volatile uint8_t enabled, role, state, n, rx_count, tx_count, writes, reads;
    volatile uint32_t errors, raw_error, phase_tick;
    uint32_t speed, start_tick, end_tick;
    uint8_t rx[16], tx[16];
} i2c_tcu;

static uint8_t i2c_tcu_pattern(uint8_t index)
{
    return (uint8_t)(0xA5U ^ (37U * index));
}

static void i2c_tcu_finish(uint32_t errors)
{
    i2c_tcu.errors |= errors;
    i2c_tcu.end_tick = HAL_GetTick();
    i2c_tcu.state = (i2c_tcu.errors == 0U) ? 3U : 4U;
    CLEAR_BIT(I2C3->CR2, I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN);
    CLEAR_BIT(I2C3->CR1, I2C_CR1_ACK | I2C_CR1_PE);
    i2c_slave.active = 0U;
}

static void i2c_slave_disable(void)
{
    HAL_NVIC_DisableIRQ(I2C3_EV_IRQn);
    HAL_NVIC_DisableIRQ(I2C3_ER_IRQn);
    if (hi2c3.Instance == I2C3) {
        CLEAR_BIT(I2C3->CR2, I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN);
        CLEAR_BIT(I2C3->CR1, I2C_CR1_ACK | I2C_CR1_PE);
    }
    i2c_slave.enabled = 0U;
    i2c_slave.active = 0U;
    HAL_NVIC_ClearPendingIRQ(I2C3_EV_IRQn);
    HAL_NVIC_ClearPendingIRQ(I2C3_ER_IRQn);
}

static void i2c_slave_receive(void)
{
    uint8_t data = (uint8_t)I2C3->DR;
    uint16_t next = (uint16_t)((i2c_slave.head + 1U) % RB_I2C_RX_SIZE);
    if (i2c_map.enabled && i2c_map.expect_address) {
        i2c_map.pointer = data;
        i2c_map.expect_address = 0U;
    }
    if (i2c_tcu.enabled && (i2c_tcu.role == 1U)) {
        if (i2c_tcu.rx_count < i2c_tcu.n) i2c_tcu.rx[i2c_tcu.rx_count] = data;
        else i2c_tcu.errors |= TCU_ERR_LENGTH;
        if (i2c_tcu.rx_count != 255U) ++i2c_tcu.rx_count;
    }
    ++i2c_slave.received;
    if (next == i2c_slave.tail) ++i2c_slave.dropped;
    else {
        i2c_slave.rx[i2c_slave.head] = data;
        i2c_slave.head = next;
    }
}

static void i2c_slave_transmit(void)
{
    uint8_t data = i2c_slave.filler;
    if (i2c_map.enabled) {
        data = i2c_map.bank[i2c_map.current][i2c_map.pointer];
        i2c_map.pointer = (uint8_t)(i2c_map.pointer + 1U);
    } else if (i2c_slave.tx_index < i2c_slave.tx_length)
        data = i2c_slave.tx[i2c_slave.tx_index++];
    I2C3->DR = data;
    ++i2c_slave.supplied;
    if (i2c_tcu.enabled && (i2c_tcu.role == 1U)) {
        if (i2c_tcu.tx_count < i2c_tcu.n) i2c_tcu.tx[i2c_tcu.tx_count] = data;
        if (i2c_tcu.tx_count != 255U) ++i2c_tcu.tx_count;
    }
}

/* Slave IRQs deliberately use the hardware flags directly: unknown-length
 * writes terminate on STOP or repeated START, not on a HAL fixed buffer size.
 * TX services BTF only after the initial byte, using clock stretching to avoid
 * a speculative second byte remaining in DR after the master's final NACK. */
void I2C3_EV_IRQHandler(void)
{
    uint32_t sr1, sr2;
    if (!i2c_slave.enabled) return;
    sr1 = I2C3->SR1;
    if ((sr1 & (I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_AF |
                I2C_SR1_OVR | I2C_SR1_TIMEOUT)) != 0U) {
        I2C3_ER_IRQHandler();
        return;
    }
    if ((sr1 & I2C_SR1_ADDR) != 0U) {
        /* Drain a final receive byte before clearing ADDR at repeated START. */
        if (i2c_slave.active && !i2c_slave.direction) {
            if ((I2C3->SR1 & I2C_SR1_RXNE) != 0U) i2c_slave_receive();
            if ((I2C3->SR1 & I2C_SR1_RXNE) != 0U) i2c_slave_receive();
        }
        sr2 = I2C3->SR2; /* SR1 then SR2 clears ADDR. */
        if (i2c_tcu.enabled && (i2c_tcu.role == 1U)) {
            uint8_t read = ((sr2 & I2C_SR2_TRA) != 0U) ? 1U : 0U;
            /* A second ADDR before STOP is forbidden by the TCU protocol. */
            if (i2c_slave.active ||
                (!read && ((i2c_tcu.state != 1U) || (i2c_tcu.writes != 0U))) ||
                (read && ((i2c_tcu.state != 2U) || (i2c_tcu.reads != 0U)))) {
                i2c_tcu_finish(TCU_ERR_SEQUENCE); return;
            }
            if (read && ((uint32_t)(HAL_GetTick() - i2c_tcu.phase_tick) < 100U)) {
                i2c_tcu_finish(TCU_ERR_GAP); return;
            }
            if (read) ++i2c_tcu.reads; else ++i2c_tcu.writes;
        }
        i2c_slave.direction = ((sr2 & I2C_SR2_TRA) != 0U) ? 1U : 0U;
        i2c_slave.active = 1U;
        i2c_slave.tx_index = 0U;
        CLEAR_BIT(I2C3->CR2, I2C_CR2_ITBUFEN);
        if (i2c_slave.direction) {
            ++i2c_slave.reads;
            i2c_slave_transmit();
        } else {
            ++i2c_slave.writes;
            i2c_map.expect_address = 1U;
            SET_BIT(I2C3->CR2, I2C_CR2_ITBUFEN);
        }
        return;
    }
    if (i2c_slave.active && !i2c_slave.direction) {
        if ((I2C3->SR1 & I2C_SR1_RXNE) != 0U) i2c_slave_receive();
        if ((I2C3->SR1 & I2C_SR1_RXNE) != 0U) i2c_slave_receive();
    }
    if ((sr1 & I2C_SR1_STOPF) != 0U) {
        (void)I2C3->SR1;
        SET_BIT(I2C3->CR1, I2C_CR1_PE | I2C_CR1_ACK); /* Clear STOPF. */
        CLEAR_BIT(I2C3->CR2, I2C_CR2_ITBUFEN);
        i2c_slave.active = 0U;
        if (i2c_tcu.enabled && (i2c_tcu.role == 1U)) {
            uint8_t index;
            if (i2c_slave.direction && (i2c_tcu.state == 5U)) {
                CLEAR_BIT(I2C3->CR1, I2C_CR1_ACK);
                return;
            }
            if (i2c_slave.direction || (i2c_tcu.state != 1U) || (i2c_tcu.rx_count != i2c_tcu.n)) {
                i2c_tcu_finish(TCU_ERR_LENGTH | TCU_ERR_SEQUENCE); return;
            }
            /* Echo the actual receive buffer, even if the pattern is wrong. */
            memcpy(i2c_slave.tx, i2c_tcu.rx, i2c_tcu.n);
            i2c_slave.tx_length = i2c_tcu.n;
            for (index = 0U; index < i2c_tcu.n; ++index)
                if (i2c_tcu.rx[index] != i2c_tcu_pattern(index)) i2c_tcu.errors |= TCU_ERR_DATA;
            i2c_tcu.phase_tick = HAL_GetTick();
            i2c_tcu.state = 2U;
        }
    } else if (i2c_slave.active && i2c_slave.direction &&
               ((sr1 & I2C_SR1_BTF) != 0U) && ((sr1 & I2C_SR1_AF) == 0U)) {
        if (i2c_tcu.enabled && (i2c_tcu.tx_count >= i2c_tcu.n)) {
            i2c_tcu_finish(TCU_ERR_LENGTH); return; /* Master ACKed last byte. */
        }
        i2c_slave_transmit();
    }
}

void I2C3_ER_IRQHandler(void)
{
    uint32_t flags;
    if (!i2c_slave.enabled) return;
    flags = I2C3->SR1 & (I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_AF |
                         I2C_SR1_OVR | I2C_SR1_TIMEOUT);
    /* AF is the master's normal end-of-read NACK, not a bus failure. */
    i2c_slave.errors |= flags & ~I2C_SR1_AF;
    if (i2c_tcu.enabled && (i2c_tcu.role == 1U)) {
        if ((flags & ~I2C_SR1_AF) != 0U) {
            i2c_tcu.raw_error |= flags & ~I2C_SR1_AF;
            i2c_tcu_finish(TCU_ERR_BUS);
        } else if ((flags & I2C_SR1_AF) != 0U) {
            if (!i2c_slave.direction || (i2c_tcu.state != 2U) || (i2c_tcu.tx_count != i2c_tcu.n))
                i2c_tcu.errors |= TCU_ERR_LENGTH;
            CLEAR_BIT(I2C3->CR1, I2C_CR1_ACK);
            i2c_tcu.state = 5U; /* Final NACK is normal; still require STOP/idle. */
            i2c_tcu.phase_tick = HAL_GetTick();
        }
    }
    I2C3->SR1 = ~flags; /* These error flags are cleared by writing zero. */
    CLEAR_BIT(I2C3->CR2, I2C_CR2_ITBUFEN);
    i2c_slave.active = 0U;
    if (!i2c_tcu.enabled) SET_BIT(I2C3->CR1, I2C_CR1_ACK);
}
static CAN_HandleTypeDef hcan1;
static uint8_t spi_data_bits = 8U;
static uint8_t spi_three_wire;
static ADC_HandleTypeDef hadc1;
static DAC_HandleTypeDef hdac;
static uint8_t power_initialized;
static uint8_t power_enabled;
static uint16_t power_dac_code;
static uint8_t power_dac_gpio_high;
static uint16_t power_target_mv;
static uint8_t power_closed_loop;
static uint8_t power_save_pending;
static uint32_t power_save_tick;
static uint32_t power_control_tick;

/* Sector 11 is reserved for persistent settings by the Keil IROM limit.
   Records are appended and the sector is erased only after all slots are
   consumed, avoiding one sector erase for every voltage-slider movement. */
#define RB_CONFIG_ADDRESS       0x080E0000UL
#define RB_CONFIG_END           0x08100000UL
#define RB_CONFIG_MAGIC         0x36504252UL /* "RBP6": persistent closed-loop voltage target. */
#define RB_POWER_DEFAULT_DAC    1588U        /* Approx. 3.3 V from the measured six-point fit. */
#define RB_POWER_DEFAULT_MV     3300U
#define RB_POWER_SAVE_DELAY_MS  1500U
#define RB_POWER_CONTROL_MS     20U
#define RB_POWER_STABLE_MS      100U          /* Reduce ADC load after convergence. */
#define RB_POWER_ADC_FULL_MV    6600U        /* PA1 uses the board's nominal 2:1 divider. */
#define RB_POWER_DEADBAND_ADC   4            /* About 6.4 mV at VOUT. */
#define RB_POWER_MAX_STEP       64

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    uint32_t data;
    uint32_t data_inverse;
} rb_power_record_t;

static uint8_t power_record_valid(const rb_power_record_t *record)
{
    uint16_t dac = (uint16_t)(record->data & 0x0FFFU);
    uint16_t target_mv = (uint16_t)((record->data >> 12U) & 0x1FFFU);
    uint8_t closed_loop = (uint8_t)((record->data >> 25U) & 1U);
    return (record->magic == RB_CONFIG_MAGIC) &&
           (record->data_inverse == ~record->data) && (dac <= 4095U) &&
           (!closed_loop || ((target_mv >= 550U) && (target_mv <= 5000U)));
}

static const rb_power_record_t *power_find_latest(uint32_t *next_address)
{
    const rb_power_record_t *latest = 0;
    uint32_t address;
    *next_address = RB_CONFIG_END;
    for (address = RB_CONFIG_ADDRESS;
         address + sizeof(rb_power_record_t) <= RB_CONFIG_END;
         address += sizeof(rb_power_record_t)) {
        const rb_power_record_t *record = (const rb_power_record_t *)address;
        /* Magic is committed last. A reset during programming can therefore
           leave magic erased while the remaining words are not erased. Skip
           that poisoned slot instead of trying to program over zero bits. */
        if ((record->magic == 0xFFFFFFFFUL) &&
            (record->sequence == 0xFFFFFFFFUL) &&
            (record->data == 0xFFFFFFFFUL) &&
            (record->data_inverse == 0xFFFFFFFFUL)) {
            *next_address = address;
            break;
        }
        if (power_record_valid(record) &&
            ((latest == 0) || ((int32_t)(record->sequence - latest->sequence) > 0))) latest = record;
    }
    return latest;
}

static uint8_t power_save(void)
{
    const rb_power_record_t *latest;
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t error = 0U, address, sequence = 1U;
    /* Persist the voltage target. RBLink restores it enabled after reset so
       VPOWER can supply translator VCCB when no external VREF is present. */
    uint32_t data = (uint32_t)power_dac_code |
                    ((uint32_t)power_target_mv << 12U) |
                    ((uint32_t)power_closed_loop << 25U);
    latest = power_find_latest(&address);
    if (latest != 0) sequence = latest->sequence + 1U;
    if (address + sizeof(rb_power_record_t) > RB_CONFIG_END) {
        erase.TypeErase = FLASH_TYPEERASE_SECTORS; erase.Sector = FLASH_SECTOR_11;
        erase.NbSectors = 1U; erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
        HAL_FLASH_Unlock();
        if (HAL_FLASHEx_Erase(&erase, &error) != HAL_OK) { HAL_FLASH_Lock(); return 0U; }
        HAL_FLASH_Lock(); address = RB_CONFIG_ADDRESS;
    }
    HAL_FLASH_Unlock();
    /* Commit magic last, so loss of power can only leave an ignored record. */
    if ((HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address + 4U, sequence) != HAL_OK) ||
        (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address + 8U, data) != HAL_OK) ||
        (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address + 12U, ~data) != HAL_OK) ||
        (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, RB_CONFIG_MAGIC) != HAL_OK)) {
        HAL_FLASH_Lock(); return 0U;
    }
    HAL_FLASH_Lock(); return 1U;
}

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

    /* V3.0.30: [CONFIG][speed_Hz:u32][mode:0..3][lsb_first:0/1]
       [data_bits:8/16][three_wire:0/1]. Keep the original 7-byte request
       compatible by defaulting it to 8-bit, full-duplex operation. */
    if ((len != 7U) && (len != 9U)) return LT_BAD_LENGTH;
    speed = get_u32(&p[1]);
    if ((speed < 164063U) || (speed > 21000000U) ||
        (p[5] > 3U) || (p[6] > 1U)) return LT_BAD_LENGTH;
    spi_data_bits = (len == 9U) ? p[7] : 8U;
    spi_three_wire = (len == 9U) ? p[8] : 0U;
    if (((spi_data_bits != 8U) && (spi_data_bits != 16U)) ||
        (spi_three_wire > 1U)) return LT_BAD_LENGTH;

    __HAL_RCC_GPIOB_CLK_ENABLE();
    /* Always release chip select before reconfiguring clock polarity. */
    HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN, GPIO_PIN_SET);
    __HAL_RCC_SPI2_CLK_ENABLE();
    /* Online speed/mode changes are supported. Stop the previous instance
       cleanly before resetting the HAL handle and applying the new divider. */
    if (hspi2.Instance == SPI2) {
        if (HAL_SPI_DeInit(&hspi2) != HAL_OK) return LT_IO_ERROR;
    }
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
    hspi2.Init.Direction = spi_three_wire ? SPI_DIRECTION_1LINE : SPI_DIRECTION_2LINES;
    hspi2.Init.DataSize = (spi_data_bits == 16U) ? SPI_DATASIZE_16BIT : SPI_DATASIZE_8BIT;
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
    uint8_t flags;
    uint8_t frames;
    uint8_t i;
    if (p[0] == 2U) {
        if (len != 1U) return LT_BAD_LENGTH;
        HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN, GPIO_PIN_SET);
        /* Close the SPI peripheral but keep the target translator enabled. */
        RB_SPI_Enable(1U);
        if (hspi2.Instance == SPI2) (void)HAL_SPI_DeInit(&hspi2);
        memset(&hspi2, 0, sizeof(hspi2));
        return LT_OK;
    }
    if (p[0] == 0U) return spi_config(p, len);
    /* [TRANSFER][flags][count][data...]. flags bit0 keeps CS asserted;
       bit1 selects receive direction for 3-wire mode. */
    if ((len < 3U) || ((p[1] & ~3U) != 0U) || (p[2] > 53U) ||
        (len != (uint8_t)(3U + p[2])) ||
        (hspi2.Instance != SPI2)) return LT_BAD_LENGTH;
    flags = p[1];
    count = p[2];
    if ((spi_data_bits == 16U) && ((count & 1U) != 0U)) return LT_BAD_LENGTH;
    frames = (spi_data_bits == 16U) ? (uint8_t)(count / 2U) : count;
    /* A zero-byte transfer is an explicit CS control/recovery operation. */
    if (count == 0U) {
        HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN,
                         (flags & 1U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
        *rlen = 0U;
        return LT_OK;
    }
    memcpy(tx, &p[3], count);
    /* Host words are written in display/wire order (MSB first). HAL accesses
       16-bit buffers in native little-endian order, so swap each pair before
       and after the peripheral transaction. */
    if (spi_data_bits == 16U) {
        for (i = 0U; i < count; i += 2U) {
            uint8_t value = tx[i]; tx[i] = tx[i + 1U]; tx[i + 1U] = value;
        }
    }
    HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN, GPIO_PIN_RESET);
    if ((spi_three_wire != 0U) && ((flags & 2U) != 0U)) {
        if (HAL_SPI_Receive(&hspi2, rx, frames, LT_TIMEOUT_MS) != HAL_OK) {
            HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN, GPIO_PIN_SET);
            return LT_IO_ERROR;
        }
    } else if (spi_three_wire != 0U) {
        if (HAL_SPI_Transmit(&hspi2, tx, frames, LT_TIMEOUT_MS) != HAL_OK) {
            HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN, GPIO_PIN_SET);
            return LT_IO_ERROR;
        }
        memset(rx, 0, count);
    } else if (HAL_SPI_TransmitReceive(&hspi2, tx, rx, frames, LT_TIMEOUT_MS) != HAL_OK) {
        HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN, GPIO_PIN_SET);
        return LT_IO_ERROR;
    }
    if ((flags & 1U) == 0U) HAL_GPIO_WritePin(RB_SPI_NSS_PORT, RB_SPI_NSS_PIN, GPIO_PIN_SET);
    if ((spi_three_wire != 0U) && ((flags & 2U) == 0U)) {
        *rlen = 0U;
        return LT_OK;
    }
    if (spi_data_bits == 16U) {
        for (i = 0U; i < count; i += 2U) {
            uint8_t value = rx[i]; rx[i] = rx[i + 1U]; rx[i + 1U] = value;
        }
    }
    memcpy(r, rx, count);
    *rlen = count;
    return LT_OK;
}

static uint8_t i2c_config(const uint8_t *p, uint8_t len)
{
    GPIO_InitTypeDef gpio = {0};
    if (len != 5U) return LT_BAD_LENGTH;
    if ((get_u32(&p[1]) == 0U) || (get_u32(&p[1]) > 400000U)) return LT_BAD_LENGTH;
    i2c_tcu.enabled = 0U;
    i2c_slave_disable();
    i2c_map.enabled = 0U;
    i2c_map.pending = 0U;
    if (hi2c3.Instance == I2C3) (void)HAL_I2C_DeInit(&hi2c3);
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

static void i2c_put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8U);
    p[2] = (uint8_t)(value >> 16U); p[3] = (uint8_t)(value >> 24U);
}

static uint8_t i2c_slave_config(const uint8_t *p, uint8_t len, uint32_t speed, uint8_t register_mode)
{
    uint8_t master_config[5] = {0U};
    uint8_t status;
    /* [6, address7, filler, tx_length, tx_data...]. Reserved addresses rejected. */
    if ((len < 4U) || (p[1] < 8U) || (p[1] > 0x77U) ||
        (p[3] > RB_I2C_TX_SIZE) || (len != (uint8_t)(4U + p[3]))) return LT_BAD_LENGTH;
    if (i2c_slave.enabled && ((I2C3->SR2 & I2C_SR2_BUSY) != 0U)) return LT_BUSY;
    i2c_put_u32(&master_config[1], speed);
    status = i2c_config(master_config, sizeof(master_config));
    if (status != LT_OK) return status;
    __HAL_I2C_DISABLE(&hi2c3);
    memset(&i2c_slave, 0, sizeof(i2c_slave));
    i2c_slave.address = p[1]; i2c_slave.filler = p[2]; i2c_slave.tx_length = p[3];
    memcpy(i2c_slave.tx, &p[4], p[3]);
    /* OAR1 bit14 must stay set in 7-bit addressing mode (RM0090). */
    I2C3->OAR1 = I2C_ADDRESSINGMODE_7BIT | ((uint32_t)p[1] << 1U);
    I2C3->OAR2 = 0U;
    i2c_map.enabled = register_mode;
    i2c_map.pointer = 0U;
    i2c_slave.enabled = 1U;
    HAL_NVIC_SetPriority(I2C3_EV_IRQn, 3U, 0U);
    HAL_NVIC_SetPriority(I2C3_ER_IRQn, 3U, 0U);
    HAL_NVIC_ClearPendingIRQ(I2C3_EV_IRQn);
    HAL_NVIC_ClearPendingIRQ(I2C3_ER_IRQn);
    SET_BIT(I2C3->CR1, I2C_CR1_ACK | I2C_CR1_PE);
    SET_BIT(I2C3->CR2, I2C_CR2_ITEVTEN | I2C_CR2_ITERREN);
    HAL_NVIC_EnableIRQ(I2C3_ER_IRQn);
    HAL_NVIC_EnableIRQ(I2C3_EV_IRQn);
    return LT_OK;
}

static uint8_t i2c_map_command(const uint8_t *p, uint8_t len)
{
    uint8_t config[4], status;
    uint32_t primask;
    if (p[0] == 11U) {
        if ((len != 3U) || (p[1] < 8U) || (p[1] > 0x77U)) return LT_BAD_LENGTH;
        i2c_map.staging = (uint8_t)(1U - i2c_map.current);
        memset(i2c_map.bank[i2c_map.staging], p[2], 256U);
        i2c_map.address = p[1]; i2c_map.filler = p[2];
        i2c_map.pending = 1U;
        return LT_OK;
    }
    if (!i2c_map.pending) return LT_BAD_ACTION;
    if (p[0] == 12U) {
        if ((len < 4U) || (p[2] == 0U) || (p[2] > 48U) ||
            (len != (uint8_t)(3U + p[2])) || ((uint16_t)p[1] + p[2] > 256U)) return LT_BAD_LENGTH;
        memcpy(&i2c_map.bank[i2c_map.staging][p[1]], &p[3], p[2]);
        return LT_OK;
    }
    if ((p[0] != 13U) || (len != 1U)) return LT_BAD_LENGTH;
    /* Freeze idle detection and IRQs together before reconfiguring. Never
     * replace a map during an external master's in-progress transaction. */
    primask = __get_PRIMASK(); __disable_irq();
    if ((hi2c3.Instance == I2C3) && ((I2C3->SR2 & I2C_SR2_BUSY) != 0U)) {
        __set_PRIMASK(primask); return LT_BUSY;
    }
    i2c_slave_disable();
    i2c_map.enabled = 0U;
    i2c_map.current = i2c_map.staging;
    __set_PRIMASK(primask);
    config[0] = 6U; config[1] = i2c_map.address; config[2] = i2c_map.filler; config[3] = 0U;
    status = i2c_slave_config(config, sizeof(config), 100000U, 1U);
    if (status == LT_OK) i2c_map.pending = 0U;
    return status;
}

static uint8_t i2c_tcu_command(const uint8_t *p, uint8_t len, uint8_t *r, uint8_t *rlen)
{
    uint32_t speed, primask;
    uint8_t config[5] = {0U}, status, index;
    const uint8_t slave_config[4] = {6U, 0x50U, 0xFFU, 0U};
    if (p[0] == 9U) {
        if ((len != 7U) || (p[1] > 1U) || ((p[2] != 1U) && (p[2] != 16U))) return LT_BAD_LENGTH;
        speed = get_u32(&p[3]);
        if ((speed != 100000U) && (speed != 400000U)) return LT_BAD_LENGTH;
        if (i2c_tcu.enabled && (i2c_tcu.state != 3U) && (i2c_tcu.state != 4U)) return LT_BUSY;
        if (p[1] == 1U) status = i2c_slave_config(slave_config, sizeof(slave_config), speed, 0U);
        else { i2c_put_u32(&config[1], speed); status = i2c_config(config, sizeof(config)); }
        if (status != LT_OK) return status;
        primask = __get_PRIMASK(); __disable_irq();
        memset(&i2c_tcu, 0, sizeof(i2c_tcu));
        i2c_tcu.role = p[1]; i2c_tcu.n = p[2]; i2c_tcu.speed = speed;
        i2c_tcu.state = 1U; i2c_tcu.start_tick = HAL_GetTick();
        i2c_tcu.phase_tick = i2c_tcu.start_tick;
        if (p[1] == 0U)
            for (index = 0U; index < p[2]; ++index) i2c_tcu.tx[index] = i2c_tcu_pattern(index);
        i2c_tcu.enabled = 1U;
        __set_PRIMASK(primask);
        return LT_OK;
    }
    if ((p[0] != 10U) || (len != 1U) || !i2c_tcu.enabled) return LT_BAD_ACTION;
    primask = __get_PRIMASK(); __disable_irq();
    r[0] = 1U; r[1] = i2c_tcu.role; r[2] = i2c_tcu.state; r[3] = i2c_tcu.n;
    i2c_put_u32(&r[4], i2c_tcu.errors);
    r[8] = i2c_tcu.rx_count; r[9] = i2c_tcu.tx_count;
    r[10] = i2c_tcu.writes; r[11] = i2c_tcu.reads;
    i2c_put_u32(&r[12], i2c_tcu.raw_error);
    i2c_put_u32(&r[16], i2c_tcu.speed);
    i2c_put_u32(&r[20], ((i2c_tcu.state == 3U) || (i2c_tcu.state == 4U)
        ? i2c_tcu.end_tick : HAL_GetTick()) - i2c_tcu.start_tick);
    memcpy(&r[24], i2c_tcu.rx, 16U); memcpy(&r[40], i2c_tcu.tx, 16U);
    __set_PRIMASK(primask);
    *rlen = 56U;
    return LT_OK;
}

static uint8_t i2c_tcu_wait_idle(void)
{
    uint32_t start = HAL_GetTick();
    while ((I2C3->SR2 & I2C_SR2_BUSY) != 0U)
        if ((uint32_t)(HAL_GetTick() - start) >= 1000U) return 0U;
    return 1U;
}

static void i2c_tcu_task(void)
{
    HAL_StatusTypeDef result;
    uint8_t index;
    if (!i2c_tcu.enabled || (i2c_tcu.state == 3U) || (i2c_tcu.state == 4U)) return;
    if (i2c_tcu.role == 1U) {
        if (i2c_tcu.state == 5U) {
            if ((I2C3->SR2 & I2C_SR2_BUSY) == 0U) i2c_tcu_finish(0U);
            else if ((uint32_t)(HAL_GetTick() - i2c_tcu.phase_tick) >= 1000U)
                i2c_tcu_finish(TCU_ERR_TIMEOUT | TCU_ERR_IDLE);
        } else if ((uint32_t)(HAL_GetTick() - i2c_tcu.phase_tick) >= 10000U) {
            i2c_tcu_finish(TCU_ERR_TIMEOUT);
        }
        return;
    }
    if (i2c_tcu.state == 1U) {
        if ((I2C3->SR2 & I2C_SR2_BUSY) != 0U) { i2c_tcu_finish(TCU_ERR_IDLE); return; }
        i2c_tcu.writes = 1U;
        hi2c3.XferCount = i2c_tcu.n;
        result = HAL_I2C_Master_Transmit(&hi2c3, 0xA0U, i2c_tcu.tx, i2c_tcu.n, 1000U);
        i2c_tcu.raw_error = HAL_I2C_GetError(&hi2c3);
        i2c_tcu.tx_count = (uint8_t)(i2c_tcu.n - hi2c3.XferCount);
        if (result != HAL_OK) {
            i2c_tcu_finish(TCU_ERR_BUS | ((result == HAL_TIMEOUT) ? TCU_ERR_TIMEOUT : 0U)); return;
        }
        if (!i2c_tcu_wait_idle()) { i2c_tcu_finish(TCU_ERR_IDLE | TCU_ERR_TIMEOUT); return; }
        i2c_tcu.phase_tick = HAL_GetTick(); i2c_tcu.state = 2U;
    } else if ((i2c_tcu.state == 2U) && ((uint32_t)(HAL_GetTick() - i2c_tcu.phase_tick) >= 101U)) {
        i2c_tcu.reads = 1U;
        hi2c3.XferCount = i2c_tcu.n;
        result = HAL_I2C_Master_Receive(&hi2c3, 0xA0U, i2c_tcu.rx, i2c_tcu.n, 1000U);
        i2c_tcu.raw_error |= HAL_I2C_GetError(&hi2c3);
        i2c_tcu.rx_count = (uint8_t)(i2c_tcu.n - hi2c3.XferCount);
        if (result != HAL_OK) {
            i2c_tcu_finish(TCU_ERR_BUS | ((result == HAL_TIMEOUT) ? TCU_ERR_TIMEOUT : 0U)); return;
        }
        if (!i2c_tcu_wait_idle()) { i2c_tcu_finish(TCU_ERR_IDLE | TCU_ERR_TIMEOUT); return; }
        if ((i2c_tcu.rx_count != i2c_tcu.n) || (i2c_tcu.tx_count != i2c_tcu.n)) i2c_tcu.errors |= TCU_ERR_LENGTH;
        for (index = 0U; index < i2c_tcu.n; ++index)
            if (i2c_tcu.rx[index] != i2c_tcu.tx[index]) i2c_tcu.errors |= TCU_ERR_DATA;
        i2c_tcu_finish(0U);
    }
}

static uint8_t i2c_slave_command(const uint8_t *p, uint8_t len, uint8_t *r, uint8_t *rlen)
{
    uint32_t primask;
    uint8_t count = 0U;
    if (!i2c_slave.enabled) return LT_BAD_ACTION;
    if (p[0] == 7U) {
        if (i2c_map.enabled) return LT_BAD_ACTION;
        if ((len < 2U) || (p[1] > RB_I2C_TX_SIZE) || (len != (uint8_t)(2U + p[1]))) return LT_BAD_LENGTH;
        primask = __get_PRIMASK(); __disable_irq();
        if ((I2C3->SR2 & I2C_SR2_BUSY) != 0U) { __set_PRIMASK(primask); return LT_BUSY; }
        /* Do not replace bytes while an external master owns the bus. */
        memcpy(i2c_slave.tx, &p[2], p[1]);
        i2c_slave.tx_length = p[1]; i2c_slave.tx_index = 0U;
        __set_PRIMASK(primask);
        return LT_OK;
    }
    if ((p[0] != 8U) || (len != 1U)) return LT_BAD_LENGTH;
    primask = __get_PRIMASK(); __disable_irq();
    r[0] = 1U; r[1] = i2c_slave.address;
    i2c_put_u32(&r[4], i2c_slave.received);
    i2c_put_u32(&r[8], i2c_slave.supplied);
    i2c_put_u32(&r[12], i2c_slave.dropped);
    i2c_put_u32(&r[16], i2c_slave.errors);
    put_u16(&r[20], i2c_slave.writes); put_u16(&r[22], i2c_slave.reads);
    while ((count < 32U) && (i2c_slave.tail != i2c_slave.head)) {
        r[24U + count++] = i2c_slave.rx[i2c_slave.tail];
        i2c_slave.tail = (uint16_t)((i2c_slave.tail + 1U) % RB_I2C_RX_SIZE);
    }
    put_u16(&r[2], (uint16_t)((i2c_slave.head + RB_I2C_RX_SIZE - i2c_slave.tail) % RB_I2C_RX_SIZE));
    __set_PRIMASK(primask);
    *rlen = (uint8_t)(24U + count);
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
    if (len == 0U) return LT_BAD_LENGTH;
    if (p[0] == 2U) {
        if (len != 1U) return LT_BAD_LENGTH;
        i2c_tcu.enabled = 0U;
        i2c_slave_disable();
        i2c_map.enabled = 0U; i2c_map.pending = 0U;
        /* Close the controller but keep the target translator enabled. */
        RB_I2C_Enable(1U);
        if (hi2c3.Instance == I2C3) (void)HAL_I2C_DeInit(&hi2c3);
        memset(&hi2c3, 0, sizeof(hi2c3));
        return LT_OK;
    }
    if ((p[0] == 9U) || (p[0] == 10U)) return i2c_tcu_command(p, len, r, rlen);
    if (i2c_tcu.enabled) return LT_BUSY;
    if ((p[0] >= 11U) && (p[0] <= 13U)) return i2c_map_command(p, len);
    if (p[0] == 6U) return i2c_slave_config(p, len, 100000U, 0U);
    if ((p[0] == 7U) || (p[0] == 8U)) return i2c_slave_command(p, len, r, rlen);
    if (p[0] == 0U) return i2c_config(p, len);
    if ((p[0] != 1U) || i2c_slave.enabled) return LT_BAD_ACTION;
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
    uint16_t value;
    cfg.Channel = channel; cfg.Rank = 1U; cfg.SamplingTime = ADC_SAMPLETIME_144CYCLES;
    if ((HAL_ADC_ConfigChannel(&hadc1, &cfg) != HAL_OK) ||
        (HAL_ADC_Start(&hadc1) != HAL_OK) ||
        (HAL_ADC_PollForConversion(&hadc1, 10U) != HAL_OK)) {
        (void)HAL_ADC_Stop(&hadc1);
        return 0xFFFFU;
    }
    value = (uint16_t)HAL_ADC_GetValue(&hadc1);
    /* Single-shot sampling must not leave ADON set between control/status
       polls, especially after the target supply is disabled. */
    (void)HAL_ADC_Stop(&hadc1);
    return value;
}

/* Six-point feed-forward calibration. Closed-loop ADC feedback removes the
   remaining board, temperature and load error after this initial estimate. */
static uint16_t power_dac_from_mv(uint16_t target_mv)
{
    int32_t code = (5057452L - (int32_t)target_mv * 1000L + 553L) / 1107L;
    if (code < 0) return 0U;
    if (code > 4095) return 4095U;
    return (uint16_t)code;
}

static uint8_t power_set_dac(uint16_t dac_code)
{
    GPIO_InitTypeDef gpio = {0};
    if (!power_initialized || (dac_code > 4095U)) return 0U;
    if (power_dac_gpio_high) {
        /* PA4 is temporarily used as a rail-to-rail GPIO only at the low-voltage
           saturation limit. Restore the analogue function before changing DAC. */
        gpio.Pin = RB_DAC_VOUT_PIN;
        gpio.Mode = GPIO_MODE_ANALOG;
        gpio.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(GPIOA, &gpio);
        if (HAL_DAC_Start(&hdac, DAC_CHANNEL_1) != HAL_OK) return 0U;
        power_dac_gpio_high = 0U;
    }
    if (HAL_DAC_SetValue(&hdac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, dac_code) != HAL_OK) return 0U;
    power_dac_code = dac_code;
    return 1U;
}

static uint8_t power_force_dac_high(void)
{
    GPIO_InitTypeDef gpio = {0};
    if (!power_initialized) return 0U;
    if (HAL_DAC_Stop(&hdac, DAC_CHANNEL_1) != HAL_OK) return 0U;
    /* The buffered DAC stops near VDDA. A GPIO high level gains the final few
       tens of millivolts of FB injection and brings TLV759P close to its
       specified 0.55 V minimum without extra ADC sampling. */
    HAL_GPIO_WritePin(GPIOA, RB_DAC_VOUT_PIN, GPIO_PIN_SET);
    gpio.Pin = RB_DAC_VOUT_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);
    power_dac_gpio_high = 1U;
    power_dac_code = 4095U;
    return 1U;
}

static void power_off(void)
{
    /* Internal fault shutdown only. Host commands are never allowed to remove
       VPOWER because all target-side level translators require this rail. */
    /* Do not leave the feedback node driven by GPIO while target power is off. */
    if (power_dac_gpio_high) (void)power_set_dac(4095U);
    /* Remove the load-facing rail first, then its upstream boost supply. */
    HAL_GPIO_WritePin(RB_LDO_EN_PORT, RB_LDO_EN_PIN, GPIO_PIN_RESET);
    HAL_Delay(1U);
    HAL_GPIO_WritePin(RB_BOOST_EN_PORT, RB_BOOST_EN_PIN, GPIO_PIN_RESET);
    if (power_initialized) (void)HAL_ADC_Stop(&hadc1);
    power_enabled = 0U;
}

static void power_control_task(void)
{
    uint16_t measured_adc, target_adc, next_code;
    int32_t error, step, next;
    uint32_t now = HAL_GetTick();
    if (!power_enabled || !power_closed_loop ||
        ((int32_t)(now - power_control_tick) < 0)) return;
    power_control_tick = now + RB_POWER_CONTROL_MS;
    measured_adc = adc_read(ADC_CHANNEL_1);
    if (measured_adc == 0xFFFFU) return;
    target_adc = (uint16_t)(((uint32_t)power_target_mv * 4095U +
                            (RB_POWER_ADC_FULL_MV / 2U)) / RB_POWER_ADC_FULL_MV);
    error = (int32_t)measured_adc - (int32_t)target_adc;
    if ((error >= -RB_POWER_DEADBAND_ADC) && (error <= RB_POWER_DEADBAND_ADC)) {
        power_control_tick = now + RB_POWER_STABLE_MS;
        return;
    }
    if (power_dac_gpio_high) {
        /* GPIO high is the absolute low-output limit. Stay there while VOUT is
           high; return to DAC mode when the requested output must rise. */
        if (error < -RB_POWER_DEADBAND_ADC) (void)power_set_dac(4095U);
        return;
    }
    if ((power_dac_code == 4095U) && (error > RB_POWER_DEADBAND_ADC)) {
        (void)power_force_dac_high();
        return;
    }
    /* One PA1 ADC count is about 1.61 mV at VOUT; one DAC count changes VOUT
       by about 1.11 mV. A gain of 1.5 corrects most error while the per-cycle
       clamp prevents overshoot and noisy limit cycling. */
    step = (error * 3L) / 2L;
    if (step > RB_POWER_MAX_STEP) step = RB_POWER_MAX_STEP;
    if (step < -RB_POWER_MAX_STEP) step = -RB_POWER_MAX_STEP;
    next = (int32_t)power_dac_code + step;
    if (next < 0) next = 0;
    if (next > 4095) next = 4095;
    next_code = (uint16_t)next;
    if (next_code != power_dac_code) (void)power_set_dac(next_code);
}

static uint8_t power_apply(uint16_t dac_code)
{
    GPIO_InitTypeDef gpio = {0};
    DAC_ChannelConfTypeDef dcfg = {0};
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
    if ((HAL_ADC_Init(&hadc1) != HAL_OK) || (HAL_DAC_Init(&hdac) != HAL_OK)) return 0U;
    dcfg.DAC_Trigger = DAC_TRIGGER_NONE; dcfg.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
    if ((HAL_DAC_ConfigChannel(&hdac, &dcfg, DAC_CHANNEL_1) != HAL_OK) ||
        (HAL_DAC_SetValue(&hdac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, dac_code) != HAL_OK) ||
        (HAL_DAC_Start(&hdac, DAC_CHANNEL_1) != HAL_OK)) return 0U;
    power_initialized = 1U; power_dac_gpio_high = 0U; power_dac_code = dac_code;
    HAL_GPIO_WritePin(RB_BOOST_EN_PORT, RB_BOOST_EN_PIN, GPIO_PIN_SET);
    HAL_Delay(5U);
    HAL_GPIO_WritePin(RB_LDO_EN_PORT, RB_LDO_EN_PIN, GPIO_PIN_SET);
    power_enabled = 1U;
    return 1U;
}

void RB_Power_Init(void)
{
    uint32_t next_address;
    const rb_power_record_t *record = power_find_latest(&next_address);
    uint16_t dac = RB_POWER_DEFAULT_DAC;
    power_target_mv = RB_POWER_DEFAULT_MV;
    power_closed_loop = 1U;
    if (record != 0) {
        dac = (uint16_t)(record->data & 0x0FFFU);
        power_target_mv = (uint16_t)((record->data >> 12U) & 0x1FFFU);
        power_closed_loop = (uint8_t)((record->data >> 25U) & 1U);
    }
    if (!power_apply(dac)) power_off();
    /* Store the factory default on the first boot so all later starts follow
       the exact same validated restore path. */
    if ((record == 0) && power_initialized) (void)power_save();
}

void RB_Bus_Task(void)
{
    i2c_tcu_task();
    power_control_task();
    /* Avoid Flash programming stalls during the one-shot I2C exchange. */
    if (i2c_tcu.enabled && (i2c_tcu.state != 3U) && (i2c_tcu.state != 4U)) return;
    if (power_save_pending &&
        ((int32_t)(HAL_GetTick() - power_save_tick) >= 0)) {
        if (power_save()) power_save_pending = 0U;
        else power_save_tick = HAL_GetTick() + RB_POWER_SAVE_DELAY_MS;
    }
}

uint8_t rblink_platform_power(const uint8_t *p, uint8_t len, uint8_t *r, uint8_t *rlen)
{
    uint16_t dac_code;
    if (p[0] == 2U) {
        if (len != 1U) return LT_BAD_LENGTH;
        /* V3 hardware requires VPOWER continuously for its level translators.
           Keep CLOSE as a successful no-op for older host compatibility. */
        if (!power_enabled && !power_apply(power_dac_code)) return LT_IO_ERROR;
        return LT_OK;
    }
    if (p[0] == 0U) {
        /* Legacy: [CONFIG][enable][raw DAC:u16].
           Closed loop: [CONFIG][enable][target_mV:u16][mode=1]. */
        if (((len != 4U) && (len != 5U)) || (p[1] > 1U)) return LT_BAD_LENGTH;
        if (len == 5U) {
            power_target_mv = (uint16_t)p[2] | ((uint16_t)p[3] << 8U);
            if ((p[4] != 1U) || (power_target_mv < 550U) ||
                (power_target_mv > 5000U)) return LT_BAD_LENGTH;
            power_closed_loop = 1U;
            dac_code = power_dac_from_mv(power_target_mv);
        } else {
            dac_code = (uint16_t)p[2] | ((uint16_t)p[3] << 8U);
            if (dac_code > 4095U) return LT_BAD_LENGTH;
            power_closed_loop = 0U;
            power_target_mv = 0U;
        }
        /* A live closed-loop setpoint change only updates DAC; avoid
           reinitializing DAC/ADC or disturbing the enabled LDO rail. */
        if ((len == 5U) && power_initialized && power_enabled) {
            if (!power_set_dac(dac_code)) { power_off(); return LT_IO_ERROR; }
        } else if (!power_apply(dac_code)) { power_off(); return LT_IO_ERROR; }
        power_control_tick = HAL_GetTick() + RB_POWER_CONTROL_MS;
        power_save_pending = 1U;
        power_save_tick = HAL_GetTick() + RB_POWER_SAVE_DELAY_MS;
        return LT_OK;
    }
    if (len != 1U) return LT_BAD_LENGTH;
    if (!power_initialized) return LT_IO_ERROR;
    r[0] = power_enabled;
    /* An off supply reports a deterministic zero trace without starting ADC.
       This avoids three conversions for every host status poll while disabled. */
    if (!power_enabled) {
        memset(&r[1], 0, 6U);
    } else {
        /* Keep the wire response order VREF, VOUT, NTC despite the PCB pin move. */
        put_u16(&r[1], adc_read(ADC_CHANNEL_2));
        put_u16(&r[3], adc_read(ADC_CHANNEL_1));
        put_u16(&r[5], adc_read(ADC_CHANNEL_0));
        if ((r[1] == 0xFFU && r[2] == 0xFFU) ||
            (r[3] == 0xFFU && r[4] == 0xFFU) ||
            (r[5] == 0xFFU && r[6] == 0xFFU)) return LT_IO_ERROR;
    }
    /* Bytes 9..10 remain reserved for wire compatibility. PA4 is no longer
       sampled: the closed loop only needs PA1, reducing ADC work and allowing
       PA4 to become a rail-to-rail GPIO at the 0.55 V saturation limit. */
    r[9] = 0xFFU; r[10] = 0xFFU;
    put_u16(&r[7], power_dac_code);
    put_u16(&r[11], power_target_mv);
    r[13] = power_closed_loop;
    *rlen = 14U;
    return LT_OK;
}
