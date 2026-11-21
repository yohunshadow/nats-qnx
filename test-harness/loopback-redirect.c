/*
 * loopback-redirect.so: LD_PRELOAD shim (QNX target, test-only).
 *
 * Redirects IPv4 connect() calls aimed at 127.0.0.1:<port> to
 * $NATS_LOOPBACK_REDIRECT (default 10.0.2.2, the QEMU user-net alias of the
 * dev host's loopback).  Together with nats-server-remote this lets the
 * unmodified upstream nats.c tests -- which assume a local nats-server on
 * 127.0.0.1 -- talk to real nats-server processes running on the host.
 * A closed port on the host still yields ECONNREFUSED in the guest, so
 * "no server running" test cases keep their normal semantics.
 *
 * Ports that the *same process* has bind()-ed itself (the upstream tests'
 * in-process "mockup servers") are NOT redirected, so those tests still talk
 * to their own local listener. The port is released again when that socket
 * is close()d, so a later real server on the same port is reached normally.
 *
 * Copyright 2026 nats-qnx authors. Licensed under the Apache License 2.0.
 */
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int (*connect_fn)(int, const struct sockaddr *, socklen_t);
typedef int (*bind_fn)(int, const struct sockaddr *, socklen_t);
typedef int (*close_fn)(int);

#define MAX_TRACKED_FD 4096
static volatile unsigned short fd_port[MAX_TRACKED_FD];

/* one bit per TCP/UDP port bound locally by this process */
static volatile unsigned char local_ports[65536 / 8];

static int port_of(const struct sockaddr *a)
{
    if (a == NULL) return -1;
    if (a->sa_family == AF_INET)  return ntohs(((const struct sockaddr_in *)a)->sin_port);
    if (a->sa_family == AF_INET6) return ntohs(((const struct sockaddr_in6 *)a)->sin6_port);
    return -1;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    static bind_fn real_bind = NULL;
    int p, rc;
    if (real_bind == NULL)
        real_bind = (bind_fn)dlsym(RTLD_NEXT, "bind");
    rc = real_bind(fd, addr, len);
    p = port_of(addr);
    if (rc == 0 && p > 0) {
        __sync_fetch_and_or(&local_ports[p / 8], (unsigned char)(1u << (p % 8)));
        if (fd >= 0 && fd < MAX_TRACKED_FD)
            fd_port[fd] = (unsigned short)p;
    }
    return rc;
}

int close(int fd)
{
    static close_fn real_close = NULL;
    if (real_close == NULL)
        real_close = (close_fn)dlsym(RTLD_NEXT, "close");
    if (fd >= 0 && fd < MAX_TRACKED_FD && fd_port[fd] != 0) {
        int p = fd_port[fd];
        fd_port[fd] = 0;
        __sync_fetch_and_and(&local_ports[p / 8], (unsigned char)~(1u << (p % 8)));
    }
    return real_close(fd);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    static connect_fn real = NULL;
    static struct in_addr target;
    static int init = 0;
    if (!init) {
        const char *t = getenv("NATS_LOOPBACK_REDIRECT");
        real = (connect_fn)dlsym(RTLD_NEXT, "connect");
        inet_pton(AF_INET, (t && *t) ? t : "10.0.2.2", &target);
        init = 1;
    }
    if (addr != NULL && addr->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
        int p = ntohs(in->sin_port);
        if (in->sin_addr.s_addr == htonl(INADDR_LOOPBACK)
            && !(local_ports[p / 8] & (1u << (p % 8)))) {
            struct sockaddr_in r;
            memcpy(&r, in, sizeof(r));
            r.sin_addr = target;
            return real(fd, (struct sockaddr *)&r, sizeof(r));
        }
    }
    return real(fd, addr, len);
}
