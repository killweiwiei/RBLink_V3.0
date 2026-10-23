#ifndef RBLINK_DEBUG_H
#define RBLINK_DEBUG_H

#include <stdint.h>

#ifndef RBLINK_DEBUG_ENABLE
#define RBLINK_DEBUG_ENABLE 0U
#endif

#if (RBLINK_DEBUG_ENABLE != 0U)

void RB_Debug_Init(void);
void RB_Debug_Task(void);

/* Records one SWD transaction without printing inside the timing-critical
   bit-bang loop. kind is R (read), W (write), or E (invalid ACK/error). */
void RB_Debug_SWDTrace(uint8_t request, uint8_t ack, uint8_t kind, uint32_t value);

/* Records one complete JTAG IDCODE scan. primary is the configured TDO input
   (PD10), alternate is the optional SWO/TDO input (PD9). The record is queued
   and printed later by RB_Debug_Task(), outside the JTAG timing loop. */
void RB_Debug_JTAGTrace(uint32_t primary, uint32_t alternate);

/* Records one 35-bit JTAG-DP/AP scan after the TAP has returned to idle.
   phase is '1' for the first scan and '2' for the ACK=000 pipeline retry. */
void RB_Debug_JTAGTransferTrace(uint8_t request, uint8_t ack, uint8_t phase,
                               uint32_t value);

#else

/* Production builds discard diagnostic UART and trace calls at preprocessing
   time, so they consume neither GPIO/UART resources nor transfer-loop time. */
#define RB_Debug_Init()                                      ((void)0)
#define RB_Debug_Task()                                      ((void)0)
#define RB_Debug_SWDTrace(request, ack, kind, value)          ((void)0)
#define RB_Debug_JTAGTrace(primary, alternate)                ((void)0)
#define RB_Debug_JTAGTransferTrace(request, ack, phase, value) ((void)0)

#endif

#endif
