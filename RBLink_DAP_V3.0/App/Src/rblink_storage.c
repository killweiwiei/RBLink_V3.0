/* External SPI NOR partition service exposed by CMSIS-DAP vendor command 0x89.
 *
 * Only logical offsets inside the algorithm and offline-image partitions are
 * accepted.  The reserved RBFS metadata area is deliberately unreachable so
 * a malformed or old host cannot overwrite future directory structures.
 */
#include "rblink_storage.h"
#include "rblink_nor.h"

#define RB_OK          0x00U
#define RB_BAD_LENGTH  0x01U
#define RB_BAD_ACTION  0x02U
#define RB_IO_ERROR    0x03U

#define RB_ACTION_INFO     0U
#define RB_ACTION_TRANSFER 1U
#define RB_ACTION_CLOSE    2U
#define RB_STORAGE_READ    1U
#define RB_STORAGE_WRITE   2U
#define RB_STORAGE_ERASE4K 3U
#define RB_STORAGE_STATUS  4U
#define RB_STORAGE_ERASE4K_START 5U
#define RB_STORAGE_MAX_IO  48U

#define RB_STORAGE_DIAG_NOT_READY 1U
#define RB_STORAGE_DIAG_READ      2U
#define RB_STORAGE_DIAG_WRITE     3U
#define RB_STORAGE_DIAG_ERASE     4U

static uint8_t io_error(uint8_t *r, uint8_t *rlen, uint8_t diagnostic)
{
  r[0] = diagnostic;
  *rlen = 1U;
  return RB_IO_ERROR;
}

static uint32_t get_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
         ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static void put_u16(uint8_t *p, uint16_t value)
{
  p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *p, uint32_t value)
{
  p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8U);
  p[2] = (uint8_t)(value >> 16U); p[3] = (uint8_t)(value >> 24U);
}

static uint8_t resolve_region(uint8_t region, uint32_t offset, uint32_t length,
                              uint32_t *physical)
{
  uint32_t base, size;
  if (region == RB_STORAGE_REGION_ALGORITHM) {
    base = RB_STORAGE_ALGORITHM_BASE; size = RB_STORAGE_ALGORITHM_SIZE;
  } else if (region == RB_STORAGE_REGION_OFFLINE) {
    base = RB_STORAGE_OFFLINE_BASE; size = RB_STORAGE_OFFLINE_SIZE;
  } else return 0U;
  if ((offset > size) || (length > (size - offset))) return 0U;
  *physical = base + offset;
  return 1U;
}

static uint8_t storage_info(uint8_t *r, uint8_t *rlen)
{
  uint32_t jedec = RB_NOR_ReadJEDEC();
  r[0] = 1U; /* layout version */
  /* bit0: ready, bit1: non-blocking erase/status commands supported */
  r[1] = RB_NOR_IsReady() ? 3U : 2U;
  r[2] = (uint8_t)(jedec >> 16U); /* manufacturer */
  r[3] = (uint8_t)(jedec >> 8U);  /* memory type */
  r[4] = (uint8_t)jedec;          /* density */
  put_u32(&r[5], RB_NOR_Capacity());
  put_u16(&r[9], 256U);
  put_u16(&r[11], 4096U);
  put_u32(&r[13], RB_STORAGE_ALGORITHM_BASE);
  put_u32(&r[17], RB_STORAGE_ALGORITHM_SIZE);
  put_u32(&r[21], RB_STORAGE_OFFLINE_BASE);
  put_u32(&r[25], RB_STORAGE_OFFLINE_SIZE);
  r[29] = RB_STORAGE_MAX_IO;
  r[30] = RB_STORAGE_MAX_IO;
  *rlen = 31U;
  /* Identification is diagnostic and must still reach the host when the
     fitted part is absent or has the wrong density. Mutating operations remain
     blocked by the NOR driver's zero-capacity range check. */
  return RB_OK;
}

uint8_t rblink_platform_storage(const uint8_t *p, uint8_t len,
                                uint8_t *r, uint8_t *rlen)
{
  uint8_t operation, region, count;
  uint32_t offset, physical;
  if ((p == 0) || (r == 0) || (rlen == 0) || (len == 0U)) return RB_BAD_LENGTH;
  *rlen = 0U;
  if (p[0] == RB_ACTION_INFO) {
    if (len != 1U) return RB_BAD_LENGTH;
    return storage_info(r, rlen);
  }
  if (p[0] == RB_ACTION_CLOSE) return (len == 1U) ? RB_OK : RB_BAD_LENGTH;
  if (p[0] != RB_ACTION_TRANSFER) return RB_BAD_ACTION;
  if (len < 7U) return RB_BAD_LENGTH;
  operation = p[1]; region = p[2]; offset = get_u32(&p[3]);
  if (!RB_NOR_IsReady()) return io_error(r, rlen, RB_STORAGE_DIAG_NOT_READY);
  if (operation == RB_STORAGE_STATUS) {
    if (len != 7U) return RB_BAD_LENGTH;
    r[0] = RB_NOR_IsBusy(); *rlen = 1U; return RB_OK;
  }
  if (operation == RB_STORAGE_ERASE4K_START) {
    if ((len != 7U) || ((offset & 0xFFFU) != 0U) ||
        !resolve_region(region, offset, 4096U, &physical)) return RB_BAD_LENGTH;
    return RB_NOR_Erase4KStart(physical) ? RB_OK : io_error(r, rlen, RB_STORAGE_DIAG_ERASE);
  }
  if (operation == RB_STORAGE_ERASE4K) {
    if ((len != 7U) || ((offset & 0xFFFU) != 0U) ||
        !resolve_region(region, offset, 4096U, &physical)) return RB_BAD_LENGTH;
    return RB_NOR_Erase4K(physical) ? RB_OK : io_error(r, rlen, RB_STORAGE_DIAG_ERASE);
  }
  if (len < 8U) return RB_BAD_LENGTH;
  count = p[7];
  if ((count == 0U) || (count > RB_STORAGE_MAX_IO) ||
      !resolve_region(region, offset, count, &physical)) return RB_BAD_LENGTH;
  if (operation == RB_STORAGE_READ) {
    if (len != 8U) return RB_BAD_LENGTH;
    if (!RB_NOR_Read(physical, r, count)) return io_error(r, rlen, RB_STORAGE_DIAG_READ);
    *rlen = count; return RB_OK;
  }
  if (operation == RB_STORAGE_WRITE) {
    if (len != (uint8_t)(8U + count)) return RB_BAD_LENGTH;
    return RB_NOR_Program(physical, &p[8], count) ? RB_OK : io_error(r, rlen, RB_STORAGE_DIAG_WRITE);
  }
  return RB_BAD_ACTION;
}
