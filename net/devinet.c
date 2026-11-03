// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/devinet.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the interface and the route through it, as a socket is asked about
 * them: what a dhcp client sets once it has a lease, and what anything
 * reads to know where the machine is.
 */

#include <velvet/netif.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/string.h>
#include <asm/uaccess.h>
#include <uapi/velvet/errno.h>
#include <uapi/velvet/if.h>
#include <uapi/velvet/route.h>
#include <uapi/velvet/sockios.h>

/* the one interface there is, by the name programs expect the first to have */
#define IF_NAME  "eth0"
#define IF_INDEX 1
#define IF_MTU   1500

static uint32_t swap32(uint32_t u_word)
{
    return (u_word >> 24) | ((u_word >> 8) & 0xff00)
           | ((u_word << 8) & 0xff0000) | (u_word << 24);
}

static void put_addr(struct sockaddr *p_out, ipv4 u_ip)
{
    struct sockaddr_in in = { .sin_family = AF_INET,
                              .sin_addr = swap32(u_ip) };
    memcpy(p_out, &in, sizeof in);
}

/* an internet address out of a sockaddr: true, or false for another family */
static bool get_addr(const struct sockaddr *p_in, ipv4 *p_ip)
{
    struct sockaddr_in in;
    memcpy(&in, p_in, sizeof in);
    *p_ip = swap32(in.sin_addr);
    return in.sin_family == AF_INET;
}

/* ones from the top and zeroes after them, and nothing else, is a netmask */
static bool is_mask(ipv4 u_mask)
{
    ipv4 u_rest = ~u_mask;
    return (u_rest & (u_rest + 1)) == 0;
}

/*
 * the netmask an address has before anybody says otherwise, by the
 * class its first byte puts it in, which is what setting an address has
 * always done to the mask
 */
static ipv4 class_mask(ipv4 u_ip)
{
    return (u_ip >> 31) == 0 ? 0xff000000
         : (u_ip >> 30) == 2 ? 0xffff0000
         : 0xffffff00;
}

static bool is_root(void)
{
    return process_uid(current_pid()) == 0;
}

/*
 * the route this machine has, the default one: a router on its own
 * network for everywhere else. a route to anywhere narrower is the
 * network the address and mask make, and is not set apart from them
 */
static int64_t route_ioctl(uint32_t u_cmd, uint64_t u_arg)
{
    struct rtentry rt;
    ipv4 u_dst = 0;
    ipv4 u_mask = 0;
    ipv4 u_gateway = 0;

    if (!is_root()) {
        return -EPERM;
    }
    if (copy_from_user(&rt, u_arg, sizeof rt) != 0) {
        return -EFAULT;
    }
    if (!get_addr(&rt.rt_dst, &u_dst) || u_dst != 0
        || (rt.rt_genmask.sa_family != 0
            && (!get_addr(&rt.rt_genmask, &u_mask) || u_mask != 0))) {
        return -EINVAL;
    }
    if (u_cmd == SIOCDELRT) {
        if (net_router() == 0) {
            return -ESRCH;
        }
        net_set_router(0);
        return 0;
    }
    if (!(rt.rt_flags & RTF_GATEWAY) || !get_addr(&rt.rt_gateway, &u_gateway)
        || u_gateway == 0) {
        return -EINVAL;
    }
    /* a router is one this machine can reach without one */
    ipv4 u_me = net_address();
    ipv4 u_net = net_netmask();
    if (u_me == 0 || (u_gateway & u_net) != (u_me & u_net)) {
        return -ENETUNREACH;
    }
    if (net_router() != 0) {
        return -EEXIST;
    }
    net_set_router(u_gateway);
    return 0;
}

int64_t net_dev_ioctl(uint32_t u_cmd, uint64_t u_arg)
{
    struct ifreq req;
    ipv4 u_ip;

    if (u_cmd == SIOCADDRT || u_cmd == SIOCDELRT) {
        return route_ioctl(u_cmd, u_arg);
    }
    if (copy_from_user(&req, u_arg, sizeof req) != 0) {
        return -EFAULT;
    }
    if (strncmp(req.ifr_name, IF_NAME, IFNAMSIZ) != 0 || !net_present()) {
        return -ENODEV;
    }
    bool b_set = u_cmd == SIOCSIFFLAGS || u_cmd == SIOCSIFADDR
                 || u_cmd == SIOCSIFNETMASK;
    if (b_set && !is_root()) {
        return -EPERM;
    }

    switch (u_cmd) {
    case SIOCGIFFLAGS:
        req.ifr_flags = (int16_t)(IFF_BROADCAST
                                  | (net_is_up() ? IFF_UP | IFF_RUNNING : 0));
        break;
    case SIOCSIFFLAGS:
        net_set_up((req.ifr_flags & IFF_UP) != 0);
        return 0;
    case SIOCGIFADDR:
    case SIOCGIFNETMASK:
    case SIOCGIFBRDADDR:
        /* an interface with no address has none of the three to give */
        if (net_address() == 0) {
            return -EADDRNOTAVAIL;
        }
        u_ip = (u_cmd == SIOCGIFADDR) ? net_address()
             : (u_cmd == SIOCGIFNETMASK) ? net_netmask()
             : (net_address() | ~net_netmask());
        put_addr(&req.ifr_addr, u_ip);
        break;
    case SIOCSIFADDR:
        if (!get_addr(&req.ifr_addr, &u_ip)) {
            return -EINVAL;
        }
        /* a new address is a new network, and the old route is not on it */
        net_set_router(0);
        net_set_address(u_ip, (u_ip == 0) ? 0 : class_mask(u_ip));
        return 0;
    case SIOCSIFNETMASK:
        if (!get_addr(&req.ifr_addr, &u_ip) || !is_mask(u_ip)) {
            return -EINVAL;
        }
        if (net_address() == 0) {
            return -EADDRNOTAVAIL;
        }
        net_set_address(net_address(), u_ip);
        return 0;
    case SIOCGIFHWADDR:
        memset(&req.ifr_hwaddr, 0, sizeof req.ifr_hwaddr);
        req.ifr_hwaddr.sa_family = ARPHRD_ETHER;
        memcpy(req.ifr_hwaddr.sa_data, net_mac()->b, MAC_LEN);
        break;
    case SIOCGIFINDEX:
        req.ifr_ifindex = IF_INDEX;
        break;
    case SIOCGIFMTU:
        req.ifr_mtu = IF_MTU;
        break;
    default:
        return -ENOTTY;
    }
    return (copy_to_user(u_arg, &req, sizeof req) == 0) ? 0 : -EFAULT;
}
