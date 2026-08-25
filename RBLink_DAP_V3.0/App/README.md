# RBLink V3.0 firmware application layer

`App` is intentionally independent from CubeMX generated files:

- `Inc/rblink_board.h`: the only board pin contract;
- `Src/rblink_board.c`: safe GPIO defaults and peripheral enable control;
- `Inc/DAP_config.h`: CMSIS-DAP fast GPIO mapping;
- `Src/rblink_bus.c`: SPI/I2C/CAN/power private-command backend;
- `Src/rblink_usb.c`: CMSIS-DAP v2 Bulk + CDC composite USB transport;
- `Src/rblink_uart.c`: non-blocking target UART bridge;
- `Src/rblink_app.c`: cooperative service loop.

CubeMX regeneration may replace `Core`, but must not replace `App`.
