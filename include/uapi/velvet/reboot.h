// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/reboot.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what reboot takes: two magic numbers, then what to do.
 */

#ifndef UAPI_VELVET_REBOOT_H
#define UAPI_VELVET_REBOOT_H

#define REBOOT_MAGIC1  0xfee1dead
#define REBOOT_MAGIC2  672274793
#define REBOOT_MAGIC2A 85072278
#define REBOOT_MAGIC2B 369367448
#define REBOOT_MAGIC2C 537993216

#define REBOOT_CMD_RESTART   0x01234567
#define REBOOT_CMD_HALT      0xcdef0123
#define REBOOT_CMD_CAD_ON    0x89abcdef
#define REBOOT_CMD_CAD_OFF   0x00000000
#define REBOOT_CMD_POWER_OFF 0x4321fedc

#endif
