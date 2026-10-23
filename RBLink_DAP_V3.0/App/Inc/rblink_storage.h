#ifndef RBLINK_STORAGE_H
#define RBLINK_STORAGE_H

#include <stdint.h>

/* Fixed external NOR layout. The first 192 KiB is reserved for redundant RBFS
   metadata; the final 1 MiB is always the target offline-firmware region. */
#define RB_STORAGE_CAPACITY       0x00400000UL
#define RB_STORAGE_ALGORITHM_BASE 0x00030000UL
#define RB_STORAGE_ALGORITHM_SIZE 0x002D0000UL
#define RB_STORAGE_OFFLINE_BASE   0x00300000UL
#define RB_STORAGE_OFFLINE_SIZE   0x00100000UL

#define RB_STORAGE_REGION_ALGORITHM 1U
#define RB_STORAGE_REGION_OFFLINE   2U

uint8_t rblink_platform_storage(const uint8_t *request, uint8_t request_len,
                                uint8_t *response, uint8_t *response_len);

#endif
