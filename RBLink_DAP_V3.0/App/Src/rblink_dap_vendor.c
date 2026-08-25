#include "DAP.h"
#include "rblink_vendor.h"

uint32_t DAP_ProcessVendorCommand(const uint8_t *request, uint8_t *response)
{
  uint8_t command = request[0];
  if ((command == ID_LTLINK_SPI) || (command == ID_LTLINK_I2C) ||
      (command == ID_LTLINK_CAN) || (command == ID_LTLINK_POWER) ||
      (command == ID_LTLINK_INFO)) {
    return ltlink_vendor_process(command, &request[1], response);
  }
  response[0] = command;
  response[1] = DAP_ERROR;
  return (1UL << 16) | 2UL;
}
