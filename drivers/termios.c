// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/termios.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * terminal modes, and what a byte means under them.
 */

#include <velvet/termios.h>
#include <velvet/string.h>
#include <uapi/velvet/signal.h>

#define CTRL(c) ((c) & 0x1f)

/* a control character that is 0 is one that is turned off */
static bool is_char(const struct termios *p_t, int i_which, uint8_t u_c)
{
    return p_t->c_cc[i_which] != 0 && p_t->c_cc[i_which] == u_c;
}

void term_default(struct termios *p_t)
{
    memset(p_t, 0, sizeof *p_t);
    p_t->c_iflag = ICRNL | IXON | IUTF8;
    p_t->c_oflag = OPOST | ONLCR;
    p_t->c_cflag = B38400 | CS8 | CREAD | HUPCL;
    p_t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE
                   | IEXTEN;
    p_t->c_cc[VINTR] = CTRL('C');
    p_t->c_cc[VQUIT] = CTRL('\\');
    p_t->c_cc[VERASE] = 0x7f;
    p_t->c_cc[VKILL] = CTRL('U');
    p_t->c_cc[VEOF] = CTRL('D');
    p_t->c_cc[VTIME] = 0;
    p_t->c_cc[VMIN] = 1;
    p_t->c_cc[VSTART] = CTRL('Q');
    p_t->c_cc[VSTOP] = CTRL('S');
    p_t->c_cc[VSUSP] = CTRL('Z');
    p_t->c_cc[VREPRINT] = CTRL('R');
    p_t->c_cc[VDISCARD] = CTRL('O');
    p_t->c_cc[VWERASE] = CTRL('W');
    p_t->c_cc[VLNEXT] = CTRL('V');
}

int term_translate(const struct termios *p_t, uint8_t u_c)
{
    if (p_t->c_iflag & ISTRIP) {
        u_c &= 0x7f;
    }
    if (u_c == '\r') {
        if (p_t->c_iflag & IGNCR) {
            return -1;
        }
        return (p_t->c_iflag & ICRNL) ? '\n' : '\r';
    }
    if (u_c == '\n' && (p_t->c_iflag & INLCR)) {
        return '\r';
    }
    return u_c;
}

int term_signal(const struct termios *p_t, uint8_t u_c)
{
    if (!(p_t->c_lflag & ISIG)) {
        return 0;
    }
    if (is_char(p_t, VINTR, u_c)) {
        return SIGINT;
    }
    if (is_char(p_t, VQUIT, u_c)) {
        return SIGQUIT;
    }
    if (is_char(p_t, VSUSP, u_c)) {
        return SIGTSTP;
    }
    return 0;
}

enum term_action term_input(const struct termios *p_t, uint8_t u_c)
{
    if (u_c == '\n' || is_char(p_t, VEOL, u_c) || is_char(p_t, VEOL2, u_c)) {
        return TERM_DELIVER;
    }
    if (is_char(p_t, VERASE, u_c)) {
        return TERM_ERASE;
    }
    if (is_char(p_t, VKILL, u_c)) {
        return TERM_KILL;
    }
    if ((p_t->c_lflag & IEXTEN) && is_char(p_t, VWERASE, u_c)) {
        return TERM_WERASE;
    }
    /*
     * on an empty line it is the end of input; with something typed it
     * delivers what there is without a newline, which is why `cat` given
     * a last line with no newline works, and surprises everybody once
     */
    if (is_char(p_t, VEOF, u_c)) {
        return TERM_EOF;
    }
    return TERM_KEEP;
}

/* a byte that would move the cursor by nothing, or somewhere odd */
static bool shown_as_control(const struct termios *p_t, uint8_t u_c)
{
    return (p_t->c_lflag & ECHOCTL)
           && (u_c < 0x20 || u_c == 0x7f) && u_c != '\n' && u_c != '\t';
}

size_t term_echo(const struct termios *p_t, uint8_t u_c, char *p_out)
{
    if (!(p_t->c_lflag & ECHO)) {
        if (u_c == '\n' && (p_t->c_lflag & ECHONL)
            && (p_t->c_lflag & ICANON)) {
            p_out[0] = '\n';
            return 1;
        }
        return 0;
    }
    if (shown_as_control(p_t, u_c)) {
        p_out[0] = '^';
        p_out[1] = (char)(u_c ^ 0x40);
        return 2;
    }
    p_out[0] = (char)u_c;
    return 1;
}

size_t term_width(const struct termios *p_t, uint8_t u_c)
{
    if (!(p_t->c_lflag & ECHO)) {
        return 0;
    }
    return shown_as_control(p_t, u_c) ? 2 : 1;
}

/*
 * a byte that moves no cursor: the ascii controls, and the second row of
 * them above 0x80 that nothing has used since
 */
static bool is_control(uint8_t u_c)
{
    return u_c < 0x20 || (u_c >= 0x7f && u_c < 0xa0);
}

size_t term_output(const struct termios *p_t, uint8_t u_c, size_t *p_column,
                   char *p_out)
{
    uint32_t u_o = p_t->c_oflag;

    if (!(u_o & OPOST)) {
        p_out[0] = (char)u_c;
        return 1;
    }
    switch (u_c) {
    case '\n':
        if (u_o & ONLRET) {
            *p_column = 0;
        }
        if (u_o & ONLCR) {
            *p_column = 0;
            p_out[0] = '\r';
            p_out[1] = '\n';
            return 2;
        }
        break;
    case '\r':
        /* a return at the left margin goes nowhere, and ONOCR leaves it out */
        if ((u_o & ONOCR) && *p_column == 0) {
            return 0;
        }
        if (u_o & OCRNL) {
            u_c = '\n';
            if (u_o & ONLRET) {
                *p_column = 0;
            }
            break;
        }
        *p_column = 0;
        break;
    case '\t': {
        size_t sz_spaces = 8 - (*p_column & 7);
        *p_column += sz_spaces;
        if ((u_o & TABDLY) == XTABS) {
            memset(p_out, ' ', sz_spaces);
            return sz_spaces;
        }
        break;
    }
    case '\b':
        if (*p_column > 0) {
            (*p_column)--;
        }
        break;
    default:
        if (is_control(u_c)) {
            break;
        }
        /*
         * a-z only: a byte of a longer utf-8 character is not a letter of
         * its own, and making it a capital would make it another one
         */
        if ((u_o & OLCUC) && u_c >= 'a' && u_c <= 'z') {
            u_c = (uint8_t)(u_c - 'a' + 'A');
        }
        /* and a byte that continues a utf-8 character takes no column */
        if (!((p_t->c_iflag & IUTF8) && (u_c & 0xc0) == 0x80)) {
            (*p_column)++;
        }
        break;
    }
    p_out[0] = (char)u_c;
    return 1;
}
