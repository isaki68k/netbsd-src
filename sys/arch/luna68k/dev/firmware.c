#ifndef _KERNEL
#include <stdint.h>
#include <unistd.h>
#endif

uint8_t xp_builtin_firmware[] = {
#include "xppcm.inc"
};
ssize_t xp_firmware_len = sizeof(xp_builtin_firmware);
uint8_t *xp_firmware = xp_builtin_firmware;
