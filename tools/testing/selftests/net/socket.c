// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/net/socket.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * sockets, from ring 3: what a socket is before the network is up.
 */

#include "../kselftest.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

int main(void)
{
    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_port = htons(7),
                                .sin_addr = { htonl(0x0a000202) } };
    int i_type = 0;
    socklen_t u_len = sizeof i_type;
    struct stat st;

    ksft_print_header();
    ksft_set_plan(7);

    int i_udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int i_tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ksft_test_result(i_udp >= 0 && i_tcp >= 0
                     && (fcntl(i_udp, F_GETFD) & FD_CLOEXEC),
                     "a socket is a descriptor, made with its flags\n");
    ksft_test_result(fstat(i_tcp, &st) == 0 && S_ISSOCK(st.st_mode),
                     "and a file, of its own kind\n");
    ksft_test_result(getsockopt(i_tcp, SOL_SOCKET, SO_TYPE, &i_type, &u_len)
                     == 0 && i_type == SOCK_STREAM && u_len == sizeof i_type,
                     "which it says\n");
    ksft_test_result(socket(AF_UNIX, SOCK_STREAM, 0) == -1
                     && errno == EAFNOSUPPORT
                     && socket(AF_INET, SOCK_RAW, 0) == -1,
                     "only the internet's are made\n");
    ksft_test_result(recv(i_tcp, &i_type, 1, 0) == -1 && errno == ENOTCONN
                     && send(i_udp, "x", 1, 0) == -1 && errno == EDESTADDRREQ,
                     "a stream not connected has nothing to read, and a "
                     "datagram nowhere to go\n");
    struct sockaddr_in peer = { 0 };
    socklen_t u_peer = sizeof peer;
    ksft_test_result(connect(i_udp, (struct sockaddr *)&addr, sizeof addr)
                     == -1 && errno == ENETUNREACH
                     && connect(i_tcp, (struct sockaddr *)&addr, sizeof addr)
                        == -1 && errno == ENETUNREACH
                     && getpeername(i_udp, (struct sockaddr *)&peer, &u_peer)
                        == -1 && errno == ENOTCONN,
                     "nowhere is reachable, and a failed connect leaves "
                     "no peer\n");
    struct timeval tv = { 0, 100000 };
    ksft_test_result(setsockopt(i_udp, SOL_SOCKET, SO_RCVTIMEO, &tv,
                                sizeof tv) == 0
                     && close(i_udp) == 0 && close(i_tcp) == 0,
                     "and closing them lets them go\n");
    ksft_finished();
}
