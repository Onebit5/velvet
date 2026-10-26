// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/sockets.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * sockets: the network as files a program opens.
 */

#include <velvet/syscalls.h>
#include <velvet/errno.h>
#include <velvet/file.h>
#include <velvet/kmalloc.h>
#include <velvet/netif.h>
#include <velvet/socket.h>
#include <velvet/pit.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/string.h>
#include <asm/uaccess.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/poll.h>
#include <uapi/velvet/socket.h>
#include <uapi/velvet/termios.h>
#include <uapi/velvet/time.h>
#include <uapi/velvet/uio.h>

/* how long a wait sleeps between looks */
#define SLICE_MS 10

/* how long a datagram waits for its next hop's address before giving up */
#define RESOLVE_MS 1000

/* where a port nobody chose is taken from */
#define EPHEMERAL_FIRST 49152
#define EPHEMERAL_LAST  65535

/*
 * one per socket file. the stack knows a socket by an owner and a
 * handle, and the owner here is the socket's own, never shared, so
 * closing one socket closes nothing of another's
 */
struct sock {
    int i_owner;
    int i_type;                 /* SOCK_STREAM or SOCK_DGRAM */
    int i_handle;               /* the stack's, or -1 for none yet */
    bool b_listening;
    bool b_shut_read;
    uint16_t u_port;            /* the local port, 0 for none yet */
    bool b_peer;                /* a datagram socket's connect() */
    ipv4 u_peer;
    uint16_t u_peer_port;
    uint64_t u_rcvtimeo_ms;     /* 0 for none */
    uint64_t u_sndtimeo_ms;
};

static int s_i_next_owner = 1 << 20;

static uint16_t swap16(uint16_t u)
{
    return (uint16_t)((u >> 8) | (u << 8));
}

static uint32_t swap32(uint32_t u_word)
{
    return (u_word >> 24) | ((u_word >> 8) & 0xff00)
           | ((u_word << 8) & 0xff0000) | (u_word << 24);
}

/* the socket an fd is open on, with a reference to put, or why not */
static struct file *sock_file(int i_fd, int64_t *p_why)
{
    struct file *p_file = process_fd_file(current_pid(), i_fd);
    if (p_file == NULL) {
        *p_why = -EBADF;
        return NULL;
    }
    if (p_file->e_type != FILE_TYPE_SOCKET) {
        file_put(p_file);
        *p_why = -ENOTSOCK;
        return NULL;
    }
    return p_file;
}

/*
 * one more look's worth of waiting: 0 to look again, or why to stop: a
 * socket that may not wait, the caller's time up, or a signal
 */
static int64_t wait_more(const struct file *p_file, int i_flags,
                         uint64_t u_deadline)
{
    if ((p_file->u_flags & O_NONBLOCK) || (i_flags & MSG_DONTWAIT)) {
        return -EAGAIN;
    }
    uint64_t u_now = pit_uptime_ms();
    if (u_now >= u_deadline) {
        return -EAGAIN;
    }
    if (process_interrupt_pending(current_pid())) {
        return -ERESTARTSYS;
    }
    uint64_t u_left = u_deadline - u_now;
    sleep_ms(u_left < SLICE_MS ? u_left : SLICE_MS);
    return 0;
}

static uint64_t deadline_after(uint64_t u_ms)
{
    return (u_ms == 0) ? UINT64_MAX : pit_uptime_ms() + u_ms;
}

/* an address out of ring 3: its ip and port, host order */
static int64_t read_addr(uint64_t u_addr, uint32_t u_len, ipv4 *p_ip,
                         uint16_t *p_port)
{
    struct sockaddr_in in;
    if (u_len < sizeof in) {
        return -EINVAL;
    }
    if (copy_from_user(&in, u_addr, sizeof in) != 0) {
        return -EFAULT;
    }
    if (in.sin_family != AF_INET) {
        return -EAFNOSUPPORT;
    }
    *p_ip = swap32(in.sin_addr);
    *p_port = swap16(in.sin_port);
    return 0;
}

/* and one back, cut to the room the caller said it had, with the full size */
static int64_t write_addr(uint64_t u_addr, uint64_t u_len, ipv4 u_ip,
                          uint16_t u_port)
{
    if (u_addr == 0) {
        return 0;
    }
    int32_t i_room = 0;
    if (copy_from_user(&i_room, u_len, sizeof i_room) != 0) {
        return -EFAULT;
    }
    if (i_room < 0) {
        return -EINVAL;
    }
    struct sockaddr_in in = {
        .sin_family = AF_INET, .sin_port = swap16(u_port),
        .sin_addr = swap32(u_ip),
    };
    size_t sz_put = ((size_t)i_room < sizeof in) ? (size_t)i_room : sizeof in;
    int32_t i_full = (int32_t)sizeof in;
    if (copy_to_user(u_addr, &in, sz_put) != 0
        || copy_to_user(u_len, &i_full, sizeof i_full) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* a datagram socket on a port: the one asked for, or a free one for 0 */
static int64_t bind_port(struct sock *p_sock, uint16_t u_port)
{
    if (p_sock->i_type != SOCK_DGRAM) {
        p_sock->u_port = u_port;
        return 0;
    }
    if (!net_is_up()) {
        return -ENETDOWN;
    }
    if (u_port != 0) {
        int i_handle = net_socket_open(p_sock->i_owner, u_port);
        if (i_handle < 0) {
            return -EADDRINUSE;
        }
        p_sock->i_handle = i_handle;
        p_sock->u_port = u_port;
        return 0;
    }
    for (uint32_t u_try = EPHEMERAL_FIRST; u_try <= EPHEMERAL_LAST; u_try++) {
        int i_handle = net_socket_open(p_sock->i_owner, (uint16_t)u_try);
        if (i_handle >= 0) {
            p_sock->i_handle = i_handle;
            p_sock->u_port = (uint16_t)u_try;
            return 0;
        }
    }
    return -EADDRNOTAVAIL;
}

/* a datagram out, given the time to find where to send it */
static int64_t send_datagram(struct file *p_file, struct sock *p_sock,
                             const void *p_data, size_t sz_len, ipv4 u_to,
                             uint16_t u_to_port, int i_flags)
{
    if (sz_len > SOCKET_DATAGRAM) {
        return -EMSGSIZE;
    }
    if (p_sock->i_handle < 0) {
        int64_t i_why = bind_port(p_sock, 0);
        if (i_why < 0) {
            return i_why;
        }
    }
    uint64_t u_deadline = pit_uptime_ms() + RESOLVE_MS;
    for (;;) {
        if (net_socket_send(p_sock->i_owner, p_sock->i_handle, u_to,
                            u_to_port, p_data, sz_len)) {
            return (int64_t)sz_len;
        }
        int64_t i_why = wait_more(p_file, i_flags & ~MSG_DONTWAIT,
                                  u_deadline);
        if (i_why == -EAGAIN) {
            return -EHOSTUNREACH;
        }
        if (i_why < 0) {
            return i_why;
        }
    }
}

/* everything of a stream out, as much as the connection will take at a time */
static int64_t send_stream(struct file *p_file, struct sock *p_sock,
                           const uint8_t *p_data, size_t sz_len, int i_flags)
{
    uint64_t u_deadline = deadline_after(p_sock->u_sndtimeo_ms);
    size_t sz_done = 0;
    if (p_sock->i_handle < 0 || p_sock->b_listening) {
        return -ENOTCONN;
    }
    while (sz_done < sz_len) {
        int64_t i_sent = net_tcp_send(p_sock->i_owner, p_sock->i_handle,
                                      p_data + sz_done, sz_len - sz_done);
        if (i_sent < 0) {
            if (sz_done > 0) {
                break;
            }
            if (!(i_flags & MSG_NOSIGNAL)) {
                process_signal(current_pid(), SIGPIPE);
            }
            return -EPIPE;
        }
        sz_done += (size_t)i_sent;
        if (i_sent > 0) {
            continue;
        }
        int64_t i_why = wait_more(p_file, i_flags, u_deadline);
        if (i_why < 0) {
            return (sz_done > 0) ? (int64_t)sz_done : i_why;
        }
    }
    return (int64_t)sz_done;
}

static int64_t send_any(struct file *p_file, const void *p_data,
                        size_t sz_len, int i_flags, const ipv4 *p_to,
                        uint16_t u_to_port)
{
    struct sock *p_sock = p_file->p_private;
    if (p_sock->i_type == SOCK_STREAM) {
        if (p_to != NULL) {
            return -EISCONN;
        }
        return send_stream(p_file, p_sock, p_data, sz_len, i_flags);
    }
    if (p_to == NULL && !p_sock->b_peer) {
        return -EDESTADDRREQ;
    }
    return send_datagram(p_file, p_sock, p_data, sz_len,
                         (p_to != NULL) ? *p_to : p_sock->u_peer,
                         (p_to != NULL) ? u_to_port : p_sock->u_peer_port,
                         i_flags);
}

/*
 * as much as is there, waiting for something to be: a datagram whole,
 * or cut to the room, or a stream's next bytes, 0 at its end
 */
static int64_t recv_any(struct file *p_file, void *p_buf, size_t sz_len,
                        int i_flags, ipv4 *p_from, uint16_t *p_from_port)
{
    struct sock *p_sock = p_file->p_private;
    uint64_t u_deadline = deadline_after(p_sock->u_rcvtimeo_ms);

    if (p_sock->i_type == SOCK_STREAM
        && (p_sock->i_handle < 0 || p_sock->b_listening)) {
        return -ENOTCONN;
    }
    for (;;) {
        int64_t i_got;
        if (p_sock->i_type == SOCK_STREAM) {
            if (p_sock->b_shut_read) {
                return 0;
            }
            i_got = net_tcp_recv(p_sock->i_owner, p_sock->i_handle, p_buf,
                                 sz_len);
            if (i_got >= 0) {
                ipv4 u_local;
                uint16_t u_local_port;
                net_tcp_ends(p_sock->i_owner, p_sock->i_handle, &u_local,
                             &u_local_port, p_from, p_from_port);
                return i_got;
            }
            /* gone altogether, which is an end like any other */
            if (net_tcp_state(p_sock->i_owner, p_sock->i_handle) < 0) {
                return 0;
            }
        } else if (p_sock->i_handle >= 0) {
            i_got = net_socket_take(p_sock->i_owner, p_sock->i_handle,
                                    p_from, p_from_port, p_buf, sz_len);
            if (i_got >= 0) {
                return i_got;
            }
        }
        int64_t i_why = wait_more(p_file, i_flags, u_deadline);
        if (i_why < 0) {
            return i_why;
        }
    }
}

static int64_t sock_read(struct file *p_file, void *p_buf, size_t sz_len)
{
    ipv4 u_from;
    uint16_t u_port;
    return recv_any(p_file, p_buf, sz_len, 0, &u_from, &u_port);
}

static int64_t sock_write(struct file *p_file, const void *p_buf,
                          size_t sz_len)
{
    return send_any(p_file, p_buf, sz_len, 0, NULL, 0);
}

static bool connected(int i_state)
{
    return i_state == TCP_ESTABLISHED || i_state == TCP_CLOSE_WAIT;
}

static uint32_t sock_poll(struct file *p_file)
{
    struct sock *p_sock = p_file->p_private;
    if (p_sock->i_type == SOCK_DGRAM) {
        bool b_in = p_sock->i_handle >= 0
                    && net_socket_pending(p_sock->i_owner,
                                          p_sock->i_handle) > 0;
        return POLLOUT | (b_in ? POLLIN : 0);
    }
    if (p_sock->i_handle < 0) {
        return POLLOUT | POLLHUP;
    }
    int i_state = net_tcp_state(p_sock->i_owner, p_sock->i_handle);
    if (p_sock->b_listening) {
        return connected(i_state) ? POLLIN : 0;
    }
    switch (i_state) {
    case TCP_SYN_SENT:
    case TCP_SYN_RECEIVED:
        return 0;
    case TCP_ESTABLISHED:
        return POLLOUT
               | (net_tcp_waiting(p_sock->i_owner, p_sock->i_handle) > 0
                  ? POLLIN : 0);
    case TCP_CLOSE_WAIT:
        return POLLIN | POLLOUT;
    default:
        return POLLIN | POLLHUP
               | (net_tcp_error(p_sock->i_owner, p_sock->i_handle) != 0
                  ? POLLERR : 0);
    }
}

static int64_t sock_ioctl(struct file *p_file, uint32_t u_cmd,
                          uint64_t u_arg)
{
    struct sock *p_sock = p_file->p_private;
    if (u_cmd != FIONREAD || p_sock->i_type != SOCK_STREAM) {
        return -ENOTTY;
    }
    int64_t i_waiting = (p_sock->i_handle >= 0)
                        ? net_tcp_waiting(p_sock->i_owner, p_sock->i_handle)
                        : 0;
    return (put_user_i32(u_arg, (int32_t)(i_waiting < 0 ? 0 : i_waiting))
            == 0) ? 0 : -EFAULT;
}

/* the last descriptor on it has closed, so the stack lets go too */
static void sock_release(struct file *p_file)
{
    struct sock *p_sock = p_file->p_private;
    if (p_sock->i_handle >= 0) {
        if (p_sock->i_type == SOCK_DGRAM) {
            net_socket_close(p_sock->i_owner, p_sock->i_handle);
        } else {
            net_tcp_shut(p_sock->i_owner, p_sock->i_handle);
        }
    }
    kfree(p_sock);
}

static const struct file_operations sock_ops = {
    .read = sock_read,
    .write = sock_write,
    .poll = sock_poll,
    .ioctl = sock_ioctl,
    .release = sock_release,
};

/* a new socket file, installed with its flags: its descriptor */
static int64_t sock_install(int i_type, int i_handle, int i_owner,
                            int i_flags)
{
    struct sock *p_sock = kmalloc(sizeof *p_sock);
    if (p_sock == NULL) {
        return -ENOMEM;
    }
    memset(p_sock, 0, sizeof *p_sock);
    p_sock->i_owner = (i_owner != 0)
                      ? i_owner
                      : __atomic_add_fetch(&s_i_next_owner, 1,
                                           __ATOMIC_RELAXED);
    p_sock->i_type = i_type;
    p_sock->i_handle = i_handle;

    struct file *p_file = file_alloc(&sock_ops, FILE_READ | FILE_WRITE,
                                     FILE_TYPE_SOCKET);
    if (p_file == NULL) {
        kfree(p_sock);
        return -ENFILE;
    }
    p_file->p_private = p_sock;
    p_file->u_flags = (i_flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0;
    return install_fd(p_file, (i_flags & SOCK_CLOEXEC) != 0);
}

int64_t sys_socket(int i_family, int i_type, int i_protocol)
{
    int i_kind = i_type & SOCK_TYPE_MASK;
    if (i_family != AF_INET) {
        return -EAFNOSUPPORT;
    }
    if (i_type & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC)) {
        return -EINVAL;
    }
    if ((i_kind == SOCK_STREAM && i_protocol != 0 && i_protocol != IPPROTO_TCP)
        || (i_kind == SOCK_DGRAM && i_protocol != 0
            && i_protocol != IPPROTO_UDP)) {
        return -EPROTONOSUPPORT;
    }
    if (i_kind != SOCK_STREAM && i_kind != SOCK_DGRAM) {
        return -ESOCKTNOSUPPORT;
    }
    return sock_install(i_kind, -1, 0, i_type);
}

int64_t sys_bind(int i_fd, uint64_t u_addr, uint32_t u_len)
{
    int64_t i_why = 0;
    struct file *p_file = sock_file(i_fd, &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    struct sock *p_sock = p_file->p_private;
    ipv4 u_ip = 0;
    uint16_t u_port = 0;
    i_why = read_addr(u_addr, u_len, &u_ip, &u_port);
    if (i_why == 0 && u_ip != INADDR_ANY && u_ip != net_address()) {
        i_why = -EADDRNOTAVAIL;
    }
    if (i_why == 0 && (p_sock->u_port != 0 || p_sock->i_handle >= 0)) {
        i_why = -EINVAL;
    }
    if (i_why == 0) {
        i_why = bind_port(p_sock, u_port);
    }
    file_put(p_file);
    return i_why;
}

int64_t sys_listen(int i_fd, int i_backlog)
{
    (void)i_backlog;
    int64_t i_why = 0;
    struct file *p_file = sock_file(i_fd, &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    struct sock *p_sock = p_file->p_private;
    if (p_sock->i_type != SOCK_STREAM) {
        i_why = -EOPNOTSUPP;
    } else if (p_sock->b_listening) {
        i_why = 0;
    } else if (p_sock->i_handle >= 0 || p_sock->u_port == 0) {
        i_why = -EINVAL;
    } else if (!net_is_up()) {
        i_why = -ENETDOWN;
    } else {
        int i_handle = net_tcp_serve(p_sock->i_owner, p_sock->u_port);
        if (i_handle < 0) {
            i_why = -EADDRINUSE;
        } else {
            p_sock->i_handle = i_handle;
            p_sock->b_listening = true;
        }
    }
    file_put(p_file);
    return i_why;
}

/*
 * a listening connection becomes the accepted one when a syn arrives,
 * there being one entry per port and no backlog. so the connection is
 * handed to a socket of its own, and this one listens again, on a new
 * entry: what a caller sees is a listener that stays a listener, with a
 * connection arriving while it has no entry refused
 */
int64_t sys_accept4(int i_fd, uint64_t u_addr, uint64_t u_len, int i_flags)
{
    if (i_flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) {
        return -EINVAL;
    }
    int64_t i_why = 0;
    struct file *p_file = sock_file(i_fd, &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    struct sock *p_sock = p_file->p_private;
    if (!p_sock->b_listening) {
        file_put(p_file);
        return -EINVAL;
    }
    uint64_t u_deadline = deadline_after(p_sock->u_rcvtimeo_ms);
    int64_t i_result;
    for (;;) {
        if (p_sock->i_handle < 0) {
            p_sock->i_handle = net_tcp_serve(p_sock->i_owner, p_sock->u_port);
        }
        int i_state = (p_sock->i_handle >= 0)
                      ? net_tcp_state(p_sock->i_owner, p_sock->i_handle) : -1;
        if (connected(i_state)) {
            int i_owner = __atomic_add_fetch(&s_i_next_owner, 1,
                                             __ATOMIC_RELAXED);
            int i_conn = p_sock->i_handle;
            ipv4 u_local;
            ipv4 u_peer = 0;
            uint16_t u_local_port;
            uint16_t u_peer_port = 0;
            net_tcp_ends(p_sock->i_owner, i_conn, &u_local, &u_local_port,
                         &u_peer, &u_peer_port);
            net_tcp_give(p_sock->i_owner, i_conn, i_owner);
            p_sock->i_handle = net_tcp_serve(p_sock->i_owner, p_sock->u_port);
            i_result = sock_install(SOCK_STREAM, i_conn, i_owner, i_flags);
            if (i_result < 0) {
                net_tcp_shut(i_owner, i_conn);
                break;
            }
            int64_t i_put = write_addr(u_addr, u_len, u_peer, u_peer_port);
            if (i_put < 0) {
                sys_close((int)i_result);
                i_result = i_put;
            }
            break;
        }
        if (i_state < 0 && p_sock->i_handle >= 0) {
            /* reset before it was taken: listen again and keep waiting */
            p_sock->i_handle = -1;
        }
        i_result = wait_more(p_file, 0, u_deadline);
        if (i_result < 0) {
            break;
        }
    }
    file_put(p_file);
    return i_result;
}

/*
 * a stream connects, and is connected when this returns 0, unless the
 * socket may not wait; a datagram socket only remembers where to send
 */
int64_t sys_connect(int i_fd, uint64_t u_addr, uint32_t u_len)
{
    int64_t i_why = 0;
    struct file *p_file = sock_file(i_fd, &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    struct sock *p_sock = p_file->p_private;
    ipv4 u_ip = 0;
    uint16_t u_port = 0;
    i_why = read_addr(u_addr, u_len, &u_ip, &u_port);
    if (i_why == 0) {
        i_why = (p_sock->b_listening) ? -EISCONN
              : (!net_is_up()) ? -ENETUNREACH
              : 0;
    }
    if (i_why == 0 && p_sock->i_type == SOCK_DGRAM) {
        if (p_sock->i_handle < 0) {
            i_why = bind_port(p_sock, 0);
        }
        if (i_why == 0) {
            p_sock->u_peer = u_ip;
            p_sock->u_peer_port = u_port;
            p_sock->b_peer = true;
        }
    } else if (i_why == 0) {
        if (p_sock->i_handle < 0) {
            p_sock->i_handle = net_tcp_open(p_sock->i_owner, u_ip, u_port);
            i_why = (p_sock->i_handle < 0) ? -EAGAIN : 0;
            if (i_why == 0 && (p_file->u_flags & O_NONBLOCK)) {
                i_why = -EINPROGRESS;
            }
        }
        while (i_why == 0) {
            int i_state = net_tcp_state(p_sock->i_owner, p_sock->i_handle);
            if (connected(i_state)) {
                break;
            }
            if (i_state <= TCP_LISTEN) {
                int i_errno = net_tcp_error(p_sock->i_owner,
                                            p_sock->i_handle);
                i_why = (i_errno != 0) ? -i_errno : -ECONNREFUSED;
                break;
            }
            i_why = wait_more(p_file, 0, UINT64_MAX);
            if (i_why == -EAGAIN) {
                i_why = -EALREADY;
            } else if (i_why == -ERESTARTSYS) {
                i_why = -EINTR;
            }
        }
    }
    file_put(p_file);
    return i_why;
}

int64_t sys_sendto(int i_fd, uint64_t u_buf, uint64_t u_len, int i_flags,
                   uint64_t u_addr, uint32_t u_addrlen)
{
    int64_t i_why = 0;
    struct file *p_file = sock_file(i_fd, &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    ipv4 u_to = 0;
    uint16_t u_port = 0;
    if (u_addr != 0) {
        i_why = read_addr(u_addr, u_addrlen, &u_to, &u_port);
    }
    void *p_data = (i_why == 0) ? kmalloc(u_len ? u_len : 1) : NULL;
    if (i_why == 0 && p_data == NULL) {
        i_why = -ENOMEM;
    }
    if (i_why == 0 && copy_from_user(p_data, u_buf, u_len) != 0) {
        i_why = -EFAULT;
    }
    if (i_why == 0) {
        i_why = send_any(p_file, p_data, u_len, i_flags,
                         (u_addr != 0) ? &u_to : NULL, u_port);
    }
    kfree(p_data);
    file_put(p_file);
    return i_why;
}

int64_t sys_recvfrom(int i_fd, uint64_t u_buf, uint64_t u_len, int i_flags,
                     uint64_t u_addr, uint64_t u_addrlen)
{
    int64_t i_why = 0;
    struct file *p_file = sock_file(i_fd, &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    /* no more can come at once than a datagram or a receive buffer holds */
    uint64_t u_most = (((struct sock *)p_file->p_private)->i_type
                       == SOCK_DGRAM) ? SOCKET_DATAGRAM : TCP_RECV_BUF;
    if (u_len > u_most) {
        u_len = u_most;
    }
    void *p_data = kmalloc(u_len ? u_len : 1);
    ipv4 u_from = 0;
    uint16_t u_port = 0;
    i_why = (p_data == NULL) ? -ENOMEM
          : recv_any(p_file, p_data, u_len, i_flags, &u_from, &u_port);
    if (i_why > 0 && copy_to_user(u_buf, p_data, (size_t)i_why) != 0) {
        i_why = -EFAULT;
    }
    if (i_why >= 0) {
        int64_t i_put = write_addr(u_addr, u_addrlen, u_from, u_port);
        i_why = (i_put < 0) ? i_put : i_why;
    }
    kfree(p_data);
    file_put(p_file);
    return i_why;
}

/*
 * a message's pieces gathered into one buffer of the kernel's, at most
 * u_max: the buffer and how much is in it
 */
static int64_t gather(const struct msghdr *p_msg, uint8_t *p_out,
                      size_t sz_max, bool b_in)
{
    size_t sz_at = 0;
    if (p_msg->msg_iovlen > IOV_MAX) {
        return -EMSGSIZE;
    }
    for (uint64_t u_i = 0; u_i < p_msg->msg_iovlen && sz_at < sz_max; u_i++) {
        struct iovec piece;
        if (copy_from_user(&piece, p_msg->msg_iov + u_i * sizeof piece,
                           sizeof piece) != 0) {
            return -EFAULT;
        }
        size_t sz_take = (piece.iov_len < sz_max - sz_at)
                         ? (size_t)piece.iov_len : sz_max - sz_at;
        if (b_in && copy_from_user(p_out + sz_at, piece.iov_base,
                                   sz_take) != 0) {
            return -EFAULT;
        }
        sz_at += sz_take;
    }
    return (int64_t)sz_at;
}

/* and what came in spread back over them */
static int64_t scatter(const struct msghdr *p_msg, const uint8_t *p_in,
                       size_t sz_len)
{
    size_t sz_at = 0;
    for (uint64_t u_i = 0; u_i < p_msg->msg_iovlen && sz_at < sz_len; u_i++) {
        struct iovec piece;
        if (copy_from_user(&piece, p_msg->msg_iov + u_i * sizeof piece,
                           sizeof piece) != 0) {
            return -EFAULT;
        }
        size_t sz_take = (piece.iov_len < sz_len - sz_at)
                         ? (size_t)piece.iov_len : sz_len - sz_at;
        if (copy_to_user(piece.iov_base, p_in + sz_at, sz_take) != 0) {
            return -EFAULT;
        }
        sz_at += sz_take;
    }
    return 0;
}

#define MSG_MAX (TCP_SEND_BUF > SOCKET_DATAGRAM ? TCP_SEND_BUF \
                                                : SOCKET_DATAGRAM)

int64_t sys_sendmsg(int i_fd, uint64_t u_msg, int i_flags)
{
    struct msghdr msg;
    if (copy_from_user(&msg, u_msg, sizeof msg) != 0) {
        return -EFAULT;
    }
    int64_t i_why = 0;
    struct file *p_file = sock_file(i_fd, &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    ipv4 u_to = 0;
    uint16_t u_port = 0;
    if (msg.msg_name != 0) {
        i_why = read_addr(msg.msg_name, msg.msg_namelen, &u_to, &u_port);
    }
    uint8_t *p_data = (i_why == 0) ? kmalloc(MSG_MAX) : NULL;
    if (i_why == 0 && p_data == NULL) {
        i_why = -ENOMEM;
    }
    if (i_why == 0) {
        i_why = gather(&msg, p_data, MSG_MAX, true);
    }
    if (i_why >= 0) {
        i_why = send_any(p_file, p_data, (size_t)i_why, i_flags,
                         (msg.msg_name != 0) ? &u_to : NULL, u_port);
    }
    kfree(p_data);
    file_put(p_file);
    return i_why;
}

int64_t sys_recvmsg(int i_fd, uint64_t u_msg, int i_flags)
{
    struct msghdr msg;
    if (copy_from_user(&msg, u_msg, sizeof msg) != 0) {
        return -EFAULT;
    }
    int64_t i_why = 0;
    struct file *p_file = sock_file(i_fd, &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    uint8_t *p_data = kmalloc(MSG_MAX);
    int64_t i_room = (p_data != NULL) ? gather(&msg, p_data, MSG_MAX, false)
                                      : -ENOMEM;
    ipv4 u_from = 0;
    uint16_t u_port = 0;
    i_why = (i_room < 0) ? i_room
          : recv_any(p_file, p_data, (size_t)i_room, i_flags, &u_from,
                     &u_port);
    if (i_why > 0) {
        int64_t i_put = scatter(&msg, p_data, (size_t)i_why);
        i_why = (i_put < 0) ? i_put : i_why;
    }
    if (i_why >= 0 && msg.msg_name != 0) {
        int64_t i_put = write_addr(msg.msg_name,
                                   u_msg + offsetof(struct msghdr,
                                                    msg_namelen),
                                   u_from, u_port);
        i_why = (i_put < 0) ? i_put : i_why;
    }
    if (i_why >= 0) {
        /* nothing comes with a message here but the message */
        uint64_t u_none = 0;
        int32_t i_flags_out = 0;
        if (copy_to_user(u_msg + offsetof(struct msghdr, msg_controllen),
                         &u_none, sizeof u_none) != 0
            || copy_to_user(u_msg + offsetof(struct msghdr, msg_flags),
                            &i_flags_out, sizeof i_flags_out) != 0) {
            i_why = -EFAULT;
        }
    }
    kfree(p_data);
    file_put(p_file);
    return i_why;
}

int64_t sys_shutdown(int i_fd, int i_how)
{
    if (i_how != SHUT_RD && i_how != SHUT_WR && i_how != SHUT_RDWR) {
        return -EINVAL;
    }
    int64_t i_why = 0;
    struct file *p_file = sock_file(i_fd, &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    struct sock *p_sock = p_file->p_private;
    if (p_sock->i_type != SOCK_STREAM || p_sock->i_handle < 0
        || p_sock->b_listening) {
        i_why = -ENOTCONN;
    } else {
        if (i_how != SHUT_WR) {
            p_sock->b_shut_read = true;
        }
        if (i_how != SHUT_RD) {
            net_tcp_shut(p_sock->i_owner, p_sock->i_handle);
        }
    }
    file_put(p_file);
    return i_why;
}

/* this end's address, or the other's */
static int64_t name_of(int i_fd, uint64_t u_addr, uint64_t u_len,
                       bool b_peer)
{
    int64_t i_why = 0;
    struct file *p_file = sock_file(i_fd, &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    struct sock *p_sock = p_file->p_private;
    ipv4 u_local = (p_sock->u_port != 0) ? net_address() : 0;
    uint16_t u_local_port = p_sock->u_port;
    ipv4 u_remote = p_sock->u_peer;
    uint16_t u_remote_port = p_sock->u_peer_port;
    bool b_has_peer = p_sock->b_peer;
    if (p_sock->i_type == SOCK_STREAM && p_sock->i_handle >= 0
        && !p_sock->b_listening) {
        b_has_peer = net_tcp_ends(p_sock->i_owner, p_sock->i_handle,
                                  &u_local, &u_local_port, &u_remote,
                                  &u_remote_port);
    }
    file_put(p_file);
    if (b_peer && !b_has_peer) {
        return -ENOTCONN;
    }
    return b_peer ? write_addr(u_addr, u_len, u_remote, u_remote_port)
                  : write_addr(u_addr, u_len, u_local, u_local_port);
}

int64_t sys_getsockname(int i_fd, uint64_t u_addr, uint64_t u_len)
{
    return name_of(i_fd, u_addr, u_len, false);
}

int64_t sys_getpeername(int i_fd, uint64_t u_addr, uint64_t u_len)
{
    return name_of(i_fd, u_addr, u_len, true);
}

static uint64_t ms_of(const struct timeval *p_tv)
{
    return (uint64_t)p_tv->tv_sec * 1000 + ((uint64_t)p_tv->tv_usec + 999)
           / 1000;
}

/*
 * the timeouts are kept. the rest asked for here are what the stack does
 * anyway: any port may be bound again at once, segments go out as they
 * are written, and a buffer is the size it is
 */
int64_t sys_setsockopt(int i_fd, int i_level, int i_name, uint64_t u_value,
                       uint32_t u_len)
{
    int64_t i_why = 0;
    struct file *p_file = sock_file(i_fd, &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    struct sock *p_sock = p_file->p_private;
    struct timeval tv;
    i_why = -ENOPROTOOPT;
    if (i_level == SOL_SOCKET
        && (i_name == SO_RCVTIMEO || i_name == SO_SNDTIMEO)) {
        if (u_len < sizeof tv) {
            i_why = -EINVAL;
        } else if (copy_from_user(&tv, u_value, sizeof tv) != 0) {
            i_why = -EFAULT;
        } else if (tv.tv_sec < 0 || tv.tv_usec < 0) {
            i_why = -EDOM;
        } else {
            *((i_name == SO_RCVTIMEO) ? &p_sock->u_rcvtimeo_ms
                                      : &p_sock->u_sndtimeo_ms) = ms_of(&tv);
            i_why = 0;
        }
    } else if ((i_level == SOL_SOCKET
                && (i_name == SO_REUSEADDR || i_name == SO_SNDBUF
                    || i_name == SO_RCVBUF))
               || (i_level == IPPROTO_TCP && i_name == TCP_NODELAY)) {
        i_why = (u_len < sizeof(int32_t)) ? -EINVAL : 0;
    }
    file_put(p_file);
    return i_why;
}

int64_t sys_getsockopt(int i_fd, int i_level, int i_name, uint64_t u_value,
                       uint64_t u_len)
{
    int64_t i_why = 0;
    struct file *p_file = sock_file(i_fd, &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    struct sock *p_sock = p_file->p_private;
    int32_t i_value = 0;
    i_why = 0;
    if (i_level != SOL_SOCKET) {
        i_why = -ENOPROTOOPT;
    } else if (i_name == SO_TYPE) {
        i_value = p_sock->i_type;
    } else if (i_name == SO_ERROR) {
        i_value = (p_sock->i_type == SOCK_STREAM && p_sock->i_handle >= 0)
                  ? net_tcp_error(p_sock->i_owner, p_sock->i_handle) : 0;
    } else if (i_name == SO_SNDBUF) {
        i_value = TCP_SEND_BUF;
    } else if (i_name == SO_RCVBUF) {
        i_value = TCP_RECV_BUF;
    } else if (i_name != SO_REUSEADDR) {
        i_why = -ENOPROTOOPT;
    }
    file_put(p_file);
    if (i_why < 0) {
        return i_why;
    }
    int32_t i_room = 0;
    if (copy_from_user(&i_room, u_len, sizeof i_room) != 0) {
        return -EFAULT;
    }
    if (i_room < (int32_t)sizeof i_value) {
        return -EINVAL;
    }
    int32_t i_size = sizeof i_value;
    if (copy_to_user(u_value, &i_value, sizeof i_value) != 0
        || copy_to_user(u_len, &i_size, sizeof i_size) != 0) {
        return -EFAULT;
    }
    return 0;
}
