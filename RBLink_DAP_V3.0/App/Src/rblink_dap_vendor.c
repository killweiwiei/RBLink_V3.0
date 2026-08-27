#include "DAP.h"
#include "rblink_vendor.h"

uint32_t DAP_ProcessVendorCommand(const uint8_t *request, uint8_t *response)
{
  uint8_t command = request[0];
  if ((command == ID_RBLINK_SPI) || (command == ID_RBLINK_I2C) ||
      (command == ID_RBLINK_CAN) || (command == ID_RBLINK_POWER) ||
      (command == ID_RBLINK_WIFI) ||
      (command == ID_RBLINK_INFO)) {
    return rblink_vendor_process(command, &request[1], response);
  }
  response[0] = command;
  response[1] = DAP_ERROR;
  return (1UL << 16) | 2UL;
}
