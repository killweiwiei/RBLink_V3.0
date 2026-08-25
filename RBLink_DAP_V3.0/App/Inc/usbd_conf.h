#ifndef RBLINK_USBD_CONF_H
#define RBLINK_USBD_CONF_H

#include "stm32f4xx_hal.h"
#include <string.h>

#define USBD_MAX_NUM_INTERFACES       3U
#define USBD_MAX_NUM_CONFIGURATION    1U
#define USBD_MAX_STR_DESC_SIZ         128U
#define USBD_SELF_POWERED             0U
#define USBD_MAX_POWER                250U
#define USBD_DEBUG_LEVEL              0U
#define USBD_SUPPORT_USER_STRING_DESC 0U
#define USBD_LPM_ENABLED              0U
#define USBD_CLASS_BOS_ENABLED        1U

#define USBD_malloc                   (void *)USBD_static_malloc
#define USBD_free                     USBD_static_free
#define USBD_memset                   memset
#define USBD_memcpy                   memcpy
#define USBD_Delay                    HAL_Delay
#define USBD_UsrLog(...)              do {} while (0)
#define USBD_ErrLog(...)              do {} while (0)
#define USBD_DbgLog(...)              do {} while (0)

void *USBD_static_malloc(uint32_t size);
void USBD_static_free(void *p);

#endif
