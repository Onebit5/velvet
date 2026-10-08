// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/cpuinfo.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what the cpu says about itself when asked nicely
 */

#ifndef ARCH_X86_64_CPUINFO_H
#define ARCH_X86_64_CPUINFO_H

/* what the cpu says about itself when asked nicely */

/* the 12-character vendor string, e.g. "GenuineIntel". buf needs 13 */
void cpu_vendor(char *buf);

/*
 * the 48-character marketing name, e.g. "AMD Ryzen 9 ...". buf needs 49.
 * older or emulated cpus may not have one, in which case you get the
 * vendor string instead
 */
void cpu_brand(char *buf);

#endif
