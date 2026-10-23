#ifndef RBLINK_BUS_H
#define RBLINK_BUS_H

/* Initialize and enable the adjustable target supply from the last valid
   Flash record. A blank MCU starts at the factory default of 3.3 V. */
void RB_Power_Init(void);

/* Performs deferred, wear-conscious persistence outside USB callbacks. */
void RB_Bus_Task(void);

/* Dedicated target I2C3 slave vectors; never dispatch these to HAL master IRQs. */
void I2C3_EV_IRQHandler(void);
void I2C3_ER_IRQHandler(void);

#endif
