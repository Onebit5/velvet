// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/serial.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * com1 uart. this is the debugging lifeline, everything gets logged here
 */

#ifndef VELVET_SERIAL_H
#define VELVET_SERIAL_H

#include <stdbool.h>

#include <stdint.h>

/* com1 uart. this is the debugging lifeline, everything gets logged here */

bool serial_init(void);
void serial_putchar(char c);
void serial_write(const char *s);

/* turn on the receive interrupt so typing into the serial console drives the shell. */
void serial_input_init(void);

/* one received byte -> the input queue. */
void serial_feed(uint8_t byte);

#endif
