// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/net/dhcp.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a dhcp client from no address at all: a broadcast out before there is
 * an address to send it from, a lease from the server qemu runs, the
 * address, netmask and route it gives set on the interface, and the
 * router pinged from it. everything is put back as it was at the end.
 * this test is installed so only root may run it.
 */

#include "../kselftest.h"

#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#define GUEST_UID    1000
#define SERVER_PORT  67
#define CLIENT_PORT  68
#define TRIES        4

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

#define OPT_MASK      1
#define OPT_ROUTER    3
#define OPT_REQUESTED 50
#define OPT_TYPE      53
#define OPT_SERVER    54
#define OPT_PARAMS    55
#define OPT_END       255

/* bootp's message, with dhcp's magic and options after it */
struct dhcp {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[312];
} __attribute__((packed));

/* what a reply said */
struct lease {
    int      i_type;
    uint32_t u_yours, u_mask, u_router, u_server;
};

static uint8_t s_arr_mac[6];
static const uint32_t s_u_xid = 0x5e1f7e57;

static struct ifreq named(void)
{
    struct ifreq req;

    memset(&req, 0, sizeof req);
    strcpy(req.ifr_name, "eth0");
    return req;
}

static bool set_addr(int i_fd, unsigned long u_cmd, uint32_t u_ip)
{
    struct ifreq req = named();
    struct sockaddr_in *p_in = (struct sockaddr_in *)&req.ifr_addr;

    p_in->sin_family = AF_INET;
    p_in->sin_addr.s_addr = u_ip;
    return ioctl(i_fd, u_cmd, &req) == 0;
}

static uint32_t get_addr(int i_fd, unsigned long u_cmd)
{
    struct ifreq req = named();

    if (ioctl(i_fd, u_cmd, &req) != 0) {
        return 0;
    }
    return ((struct sockaddr_in *)&req.ifr_addr)->sin_addr.s_addr;
}

static bool set_flags(int i_fd, short i_flags)
{
    struct ifreq req = named();

    req.ifr_flags = i_flags;
    return ioctl(i_fd, SIOCSIFFLAGS, &req) == 0;
}

static short get_flags(int i_fd)
{
    struct ifreq req = named();

    return (ioctl(i_fd, SIOCGIFFLAGS, &req) == 0) ? req.ifr_flags : -1;
}

static bool route(int i_fd, unsigned long u_cmd, uint32_t u_router)
{
    struct rtentry rt;

    memset(&rt, 0, sizeof rt);
    ((struct sockaddr_in *)&rt.rt_dst)->sin_family = AF_INET;
    ((struct sockaddr_in *)&rt.rt_genmask)->sin_family = AF_INET;
    ((struct sockaddr_in *)&rt.rt_gateway)->sin_family = AF_INET;
    ((struct sockaddr_in *)&rt.rt_gateway)->sin_addr.s_addr = u_router;
    rt.rt_flags = RTF_UP | RTF_GATEWAY;
    return ioctl(i_fd, u_cmd, &rt) == 0;
}

static size_t put_option(uint8_t *p_at, uint8_t u_code, const void *p_value,
                         uint8_t u_len)
{
    p_at[0] = u_code;
    p_at[1] = u_len;
    memcpy(p_at + 2, p_value, u_len);
    return 2 + (size_t)u_len;
}

/* a discover, or a request for what was offered */
static size_t build(struct dhcp *p_msg, uint8_t u_type,
                    const struct lease *p_offer)
{
    static const uint8_t s_arr_params[] = { OPT_MASK, OPT_ROUTER };
    size_t sz_at = 0;

    memset(p_msg, 0, sizeof *p_msg);
    p_msg->op = 1;
    p_msg->htype = 1;
    p_msg->hlen = 6;
    p_msg->xid = s_u_xid;
    p_msg->flags = htons(0x8000);           /* answer to everybody */
    memcpy(p_msg->chaddr, s_arr_mac, sizeof s_arr_mac);
    p_msg->magic = htonl(0x63825363);
    sz_at += put_option(p_msg->options + sz_at, OPT_TYPE, &u_type, 1);
    if (p_offer != NULL) {
        sz_at += put_option(p_msg->options + sz_at, OPT_REQUESTED,
                            &p_offer->u_yours, 4);
        sz_at += put_option(p_msg->options + sz_at, OPT_SERVER,
                            &p_offer->u_server, 4);
    }
    sz_at += put_option(p_msg->options + sz_at, OPT_PARAMS, s_arr_params,
                        sizeof s_arr_params);
    p_msg->options[sz_at++] = OPT_END;
    return offsetof(struct dhcp, options) + sz_at;
}

/* a reply to this client, read for what it says: true if it is one */
static bool read_reply(const struct dhcp *p_msg, ssize_t i_len,
                       struct lease *p_out)
{
    memset(p_out, 0, sizeof *p_out);
    if (i_len < (ssize_t)offsetof(struct dhcp, options) || p_msg->op != 2
        || p_msg->xid != s_u_xid || p_msg->magic != htonl(0x63825363)) {
        return false;
    }
    p_out->u_yours = p_msg->yiaddr;
    size_t sz_end = (size_t)i_len - offsetof(struct dhcp, options);
    for (size_t sz_at = 0; sz_at + 1 < sz_end; ) {
        uint8_t u_code = p_msg->options[sz_at];
        if (u_code == OPT_END) {
            break;
        }
        if (u_code == 0) {
            sz_at++;
            continue;
        }
        uint8_t u_len = p_msg->options[sz_at + 1];
        const uint8_t *p_value = &p_msg->options[sz_at + 2];
        if (sz_at + 2 + u_len > sz_end) {
            break;
        }
        if (u_code == OPT_TYPE && u_len == 1) {
            p_out->i_type = p_value[0];
        } else if (u_code == OPT_MASK && u_len == 4) {
            memcpy(&p_out->u_mask, p_value, 4);
        } else if (u_code == OPT_ROUTER && u_len >= 4) {
            memcpy(&p_out->u_router, p_value, 4);
        } else if (u_code == OPT_SERVER && u_len == 4) {
            memcpy(&p_out->u_server, p_value, 4);
        }
        sz_at += 2 + (size_t)u_len;
    }
    return p_out->i_type != 0;
}

/* a message to every machine on the wire, and the reply of the type wanted */
static bool ask(int i_fd, uint8_t u_type, const struct lease *p_offer,
                int i_want, struct lease *p_out)
{
    struct sockaddr_in all = { .sin_family = AF_INET,
                               .sin_port = htons(SERVER_PORT),
                               .sin_addr = { htonl(INADDR_BROADCAST) } };
    struct dhcp msg;

    for (int i_try = 0; i_try < TRIES; i_try++) {
        size_t sz_len = build(&msg, u_type, p_offer);
        if (sendto(i_fd, &msg, sz_len, 0, (struct sockaddr *)&all,
                   sizeof all) != (ssize_t)sz_len) {
            return false;
        }
        for (;;) {
            ssize_t i_got = recv(i_fd, &msg, sizeof msg, 0);
            if (i_got < 0) {
                break;          /* the timeout, so ask again */
            }
            if (read_reply(&msg, i_got, p_out) && p_out->i_type == i_want) {
                return true;
            }
        }
    }
    return false;
}

static bool guest_refused(int i_fd)
{
    pid_t i_child = fork();
    if (i_child == 0) {
        struct ifreq req = named();
        bool b_guest = setgid(GUEST_UID) == 0 && setuid(GUEST_UID) == 0;
        bool b_set = !set_addr(i_fd, SIOCSIFADDR, htonl(0x0a000263))
                     && errno == EPERM
                     && !set_flags(i_fd, IFF_UP) && errno == EPERM;
        bool b_read = ioctl(i_fd, SIOCGIFHWADDR, &req) == 0;
        _exit(b_guest && b_set && b_read ? 0 : 1);
    }
    int i_status = -1;
    waitpid(i_child, &i_status, 0);
    return ksft_exited(i_status, 0);
}

/* an echo request to the router, and its reply back */
static bool ping(uint32_t u_router)
{
    struct sockaddr_in to = { .sin_family = AF_INET,
                              .sin_addr = { u_router } };
    struct sockaddr_in from;
    socklen_t u_from = sizeof from;
    struct timeval tv = { 2, 0 };
    uint8_t arr_out[sizeof(struct icmphdr) + 8];
    uint8_t arr_in[64];

    int i_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (i_fd < 0) {
        return false;
    }
    setsockopt(i_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    memset(arr_out, 0, sizeof arr_out);
    struct icmphdr *p_req = (struct icmphdr *)arr_out;
    p_req->type = ICMP_ECHO;
    p_req->un.echo.sequence = htons(1);
    memcpy(arr_out + sizeof *p_req, "an echo", 8);

    bool b_ok = false;
    for (int i_try = 0; i_try < TRIES && !b_ok; i_try++) {
        if (sendto(i_fd, arr_out, sizeof arr_out, 0, (struct sockaddr *)&to,
                   sizeof to) != (ssize_t)sizeof arr_out) {
            continue;
        }
        ssize_t i_got = recvfrom(i_fd, arr_in, sizeof arr_in, 0,
                                 (struct sockaddr *)&from, &u_from);
        const struct icmphdr *p_reply = (const struct icmphdr *)arr_in;
        b_ok = i_got == (ssize_t)sizeof arr_out
               && p_reply->type == ICMP_ECHOREPLY
               && p_reply->un.echo.sequence == htons(1)
               && memcmp(arr_in + sizeof *p_reply, "an echo", 8) == 0
               && from.sin_addr.s_addr == u_router;
    }
    close(i_fd);
    return b_ok;
}

int main(void)
{
    struct ifreq req = named();
    struct lease offer;
    struct lease ack;
    struct timeval tv = { 1, 0 };
    int i_on = 1;

    ksft_print_header();
    ksft_set_plan(11);

    int i_fd = socket(AF_INET, SOCK_DGRAM, 0);
    ksft_test_result(i_fd >= 0 && ioctl(i_fd, SIOCGIFHWADDR, &req) == 0
                     && req.ifr_hwaddr.sa_family == 1
                     && (get_flags(i_fd) & IFF_UP) == 0
                     && get_addr(i_fd, SIOCGIFADDR) == 0
                     && errno == EADDRNOTAVAIL,
                     "the interface has a hardware address, and is down, "
                     "with no address\n");
    memcpy(s_arr_mac, req.ifr_hwaddr.sa_data, sizeof s_arr_mac);
    ksft_test_result(guest_refused(i_fd), "a guest may read it and not "
                     "change it\n");

    struct sockaddr_in here = { .sin_family = AF_INET,
                                .sin_port = htons(CLIENT_PORT) };
    bool b_bound = set_flags(i_fd, IFF_UP) && (get_flags(i_fd) & IFF_UP)
                   && bind(i_fd, (struct sockaddr *)&here, sizeof here) == 0
                   && setsockopt(i_fd, SOL_SOCKET, SO_RCVTIMEO, &tv,
                                 sizeof tv) == 0;
    ksft_test_result(b_bound, "brought up, a socket is bound to the "
                     "client's port\n");

    struct dhcp msg;
    struct sockaddr_in all = { .sin_family = AF_INET,
                               .sin_port = htons(SERVER_PORT),
                               .sin_addr = { htonl(INADDR_BROADCAST) } };
    size_t sz_len = build(&msg, DHCP_DISCOVER, NULL);
    ksft_test_result(sendto(i_fd, &msg, sz_len, 0, (struct sockaddr *)&all,
                            sizeof all) == -1 && errno == EACCES
                     && setsockopt(i_fd, SOL_SOCKET, SO_BROADCAST, &i_on,
                                   sizeof i_on) == 0,
                     "a datagram to everybody is refused until the socket "
                     "says it will send one\n");

    bool b_offer = ask(i_fd, DHCP_DISCOVER, NULL, DHCP_OFFER, &offer)
                   && offer.u_yours != 0 && offer.u_server != 0;
    ksft_test_result(b_offer, "a discover from no address is broadcast, and "
                     "the server offers one\n");
    bool b_ack = b_offer
                 && ask(i_fd, DHCP_REQUEST, &offer, DHCP_ACK, &ack)
                 && ack.u_yours == offer.u_yours && ack.u_mask != 0
                 && ack.u_router != 0;
    ksft_test_result(b_ack, "and the request for it is acknowledged, with a "
                     "netmask and a router\n");

    bool b_set = b_ack && set_addr(i_fd, SIOCSIFADDR, ack.u_yours)
                 && set_addr(i_fd, SIOCSIFNETMASK, ack.u_mask)
                 && route(i_fd, SIOCADDRT, ack.u_router);
    ksft_test_result(b_set && get_addr(i_fd, SIOCGIFADDR) == ack.u_yours
                     && get_addr(i_fd, SIOCGIFNETMASK) == ack.u_mask
                     && get_addr(i_fd, SIOCGIFBRDADDR)
                        == (ack.u_yours | ~ack.u_mask),
                     "the lease is the interface's: its address, its netmask, "
                     "and the broadcast they make\n");
    ksft_test_result(b_set && !route(i_fd, SIOCADDRT, ack.u_router)
                     && errno == EEXIST
                     && !set_addr(i_fd, SIOCSIFNETMASK, htonl(0xff00ff00))
                     && errno == EINVAL,
                     "one default route at a time, and a netmask has its "
                     "ones together\n");

    ksft_test_result(b_set && ping(ack.u_router),
                     "the router answers a ping from it\n");

    /* put back as it was, for the tests that expect no network */
    bool b_back = route(i_fd, SIOCDELRT, 0) && set_addr(i_fd, SIOCSIFADDR, 0)
                  && set_flags(i_fd, 0) && (get_flags(i_fd) & IFF_UP) == 0;
    ksft_test_result(b_back, "and the route, the address and the interface "
                     "taken down again\n");
    struct sockaddr_in there = { .sin_family = AF_INET, .sin_port = htons(7),
                                 .sin_addr = { ack.u_router } };
    int i_other = socket(AF_INET, SOCK_DGRAM, 0);
    ksft_test_result(i_other >= 0
                     && connect(i_other, (struct sockaddr *)&there,
                                sizeof there) == -1 && errno == ENETUNREACH,
                     "after which nothing is reachable again\n");
    if (i_other >= 0) {
        close(i_other);
    }
    close(i_fd);
    ksft_finished();
}
