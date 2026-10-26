// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/termios.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a terminal: its settings, its size, and what ioctl asks of it.
 */

#ifndef UAPI_VELVET_TERMIOS_H
#define UAPI_VELVET_TERMIOS_H

#include <stdint.h>

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TCSBRK     0x5409
#define TCXONC     0x540a
#define TCFLSH     0x540b
#define TIOCSCTTY  0x540e
#define TIOCGPGRP  0x540f
#define TIOCSPGRP  0x5410
#define TIOCOUTQ   0x5411
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD   0x541b
#define FIONBIO    0x5421
#define TIOCNOTTY  0x5422
#define TIOCGSID   0x5429
#define FIONCLEX   0x5450
#define FIOCLEX    0x5451

#define NCCS 19

struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];
};

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* c_cc */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define VEOL2    16

/* c_iflag */
#define BRKINT  0000002
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0002000
#define IMAXBEL 0020000
#define IUTF8   0040000

/* c_oflag */
#define OPOST 0000001
#define ONLCR 0000004

/* c_cflag */
#define B38400 0000017
#define CSIZE  0000060
#define CS8    0000060
#define CREAD  0000200
#define HUPCL  0002000

/* c_lflag */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define ECHOCTL 0001000
#define ECHOKE  0004000
#define IEXTEN  0100000

#endif
