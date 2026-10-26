// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/sysinfo.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what sysinfo says about the whole machine.
 */

#ifndef UAPI_VELVET_SYSINFO_H
#define UAPI_VELVET_SYSINFO_H

#include <stdint.h>

struct sysinfo {
    int64_t  uptime;            /* seconds since boot */
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint32_t pad2;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;          /* the memory counts are in units of this */
    uint32_t pad3;
};

#endif
