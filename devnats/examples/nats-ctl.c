/*
 * nats-ctl: command-line front end for the devnats devctl() API.
 *
 *   nats-ctl stats   /dev/nats/<path>
 *   nats-ctl req     /dev/nats/<path> <payload> [timeout_ms]
 *   nats-ctl jspub   /dev/nats/<path> <payload> [msg-id]
 *   nats-ctl qsub    /dev/nats/<path> <queue-group> <count> [timeout_s]
 *   nats-ctl read    /dev/nats/<path> <count> [timeout_s]   (blocking reads)
 *   nats-ctl nbread  /dev/nats/<path>                       (one O_NONBLOCK read)
 *   nats-ctl flood   /dev/nats/<path> <qmax> <wait_ms>      (queue bound/drops)
 *   nats-ctl select  /dev/nats/<path> <timeout_ms>          (select() readiness)
 *   nats-ctl poll    /dev/nats/<path> <timeout_ms>          (poll() readiness)
 *
 * Copyright 2026 nats-qnx authors. Licensed under the Apache License 2.0.
 */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>
#include "../devnats.h"

static int xopen(const char *p, int fl)
{
    int fd = open(p, fl);
    if (fd == -1) { perror(p); exit(1); }
    return fd;
}

static void print_stats(int fd)
{
    devnats_stats_t s;
    int r = devctl(fd, DCMD_NATS_GET_STATS, &s, sizeof(s), NULL);
    if (r != EOK) { fprintf(stderr, "stats: %s\n", strerror(r)); exit(1); }
    printf("server=%s connected=%u reconnects=%u opens=%u\n", s.server, s.connected,
           s.reconnects, s.opens);
    printf("global: msgs_in=%llu msgs_out=%llu bytes_in=%llu bytes_out=%llu drops=%llu\n",
           (unsigned long long)s.msgs_in, (unsigned long long)s.msgs_out,
           (unsigned long long)s.bytes_in, (unsigned long long)s.bytes_out,
           (unsigned long long)s.drops);
    printf("fd: subject=%s queue_group=%s in=%llu out=%llu drops=%llu queued=%u qmax=%u\n",
           s.subject, s.queue_group[0] ? s.queue_group : "-",
           (unsigned long long)s.ocb_msgs_in, (unsigned long long)s.ocb_msgs_out,
           (unsigned long long)s.ocb_drops, s.ocb_queued, s.ocb_qmax);
}

static void on_alarm(int s) { (void)s; }

int main(int argc, char **argv)
{
    const char *cmd, *path;
    if (argc < 3) {
        fprintf(stderr, "usage: nats-ctl stats|req|jspub|qsub|read|nbread|flood|select|poll /dev/nats/<path> ...\n");
        return 1;
    }
    cmd = argv[1];
    path = argv[2];

    if (strcmp(cmd, "stats") == 0) {
        int fd = xopen(path, O_RDONLY);
        print_stats(fd);
    } else if (strcmp(cmd, "req") == 0 && argc >= 4) {
        devnats_request_t r;
        int fd = xopen(path, O_WRONLY), rc;
        memset(&r, 0, sizeof(r));
        r.len = (uint32_t)strlen(argv[3]);
        memcpy(r.data, argv[3], r.len);
        r.timeout_ms = argc > 4 ? (uint32_t)atoi(argv[4]) : 2000;
        rc = devctl(fd, DCMD_NATS_REQUEST, &r, sizeof(r), NULL);
        if (rc != EOK) { printf("request failed: %s\n", rc == ENOENT ? "no responders" : strerror(rc)); return 1; }
        printf("reply: %.*s\n", (int)r.len, r.data);
    } else if (strcmp(cmd, "jspub") == 0 && argc >= 4) {
        devnats_jspub_t j;
        int fd = xopen(path, O_WRONLY), rc;
        memset(&j, 0, sizeof(j));
        j.len = (uint32_t)strlen(argv[3]);
        memcpy(j.data, argv[3], j.len);
        if (argc > 4) snprintf(j.msg_id, sizeof(j.msg_id), "%s", argv[4]);
        rc = devctl(fd, DCMD_NATS_JS_PUBLISH, &j, sizeof(j), NULL);
        if (rc != EOK) { printf("jspub failed: %s\n", rc == ENOENT ? "no stream for subject" : strerror(rc)); return 1; }
        printf("jspub ack: stream=%s seq=%llu duplicate=%u\n", j.stream,
               (unsigned long long)j.seq, j.duplicate);
    } else if (strcmp(cmd, "qsub") == 0 && argc >= 5) {
        char g[64] = {0}, buf[4096];
        int fd = xopen(path, O_RDONLY), rc, n = atoi(argv[4]);
        if (argc > 5) { signal(SIGALRM, on_alarm); alarm((unsigned)atoi(argv[5])); }
        snprintf(g, sizeof(g), "%s", argv[3]);
        if ((rc = devctl(fd, DCMD_NATS_SET_QGROUP, g, sizeof(g), NULL)) != EOK) {
            fprintf(stderr, "qgroup: %s\n", strerror(rc)); return 1;
        }
        printf("queue group %s on %s\n", g, path);
        fflush(stdout);
        for (int i = 0; i < n; i++) {
            ssize_t r = read(fd, buf, sizeof(buf) - 1);
            if (r <= 0) break;
            buf[r] = 0;
            printf("[%s] %s", g, buf);
            fflush(stdout);
        }
    } else if (strcmp(cmd, "read") == 0 && argc >= 4) {
        char buf[4096];
        int fd = xopen(path, O_RDONLY), n = atoi(argv[3]), i;
        if (argc > 4) { signal(SIGALRM, on_alarm); alarm((unsigned)atoi(argv[4])); }
        printf("ready\n");
        fflush(stdout);
        for (i = 0; i < n; i++) {
            ssize_t r = read(fd, buf, sizeof(buf) - 1);
            if (r < 0) { printf("read: %s\n", strerror(errno)); break; }
            if (r == 0) { printf("EOF\n"); break; }
            buf[r] = 0;
            printf("%s", buf);
            fflush(stdout);
        }
        return i == n ? 0 : 2;
    } else if (strcmp(cmd, "nbread") == 0) {
        char buf[256];
        int fd = xopen(path, O_RDONLY | O_NONBLOCK);
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r < 0) printf("read -> -1 errno=%s\n", errno == EAGAIN ? "EAGAIN" : strerror(errno));
        else printf("read -> %zd bytes\n", r);
        return 0;
    } else if (strcmp(cmd, "flood") == 0 && argc >= 5) {
        /* open for reading, shrink the queue, wait while others publish */
        uint32_t q = (uint32_t)atoi(argv[3]);
        int fd = xopen(path, O_RDONLY), rc;
        if ((rc = devctl(fd, DCMD_NATS_SET_QMAX, &q, sizeof(q), NULL)) != EOK) {
            fprintf(stderr, "qmax: %s\n", strerror(rc)); return 1;
        }
        printf("ready\n");
        fflush(stdout);
        usleep((useconds_t)atoi(argv[4]) * 1000);
        print_stats(fd);
    } else if ((strcmp(cmd, "select") == 0 || strcmp(cmd, "poll") == 0) && argc >= 4) {
        char buf[4096];
        int fd = xopen(path, O_RDONLY | O_NONBLOCK), ms = atoi(argv[3]), ready;
        printf("ready\n");
        fflush(stdout);
        if (cmd[0] == 's') {
            fd_set rf;
            struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
            FD_ZERO(&rf);
            FD_SET(fd, &rf);
            ready = select(fd + 1, &rf, NULL, NULL, &tv);
            ready = ready > 0 && FD_ISSET(fd, &rf);
        } else {
            struct pollfd pf = { .fd = fd, .events = POLLIN };
            ready = poll(&pf, 1, ms);
            ready = ready > 0 && (pf.revents & POLLIN);
        }
        if (!ready) { printf("%s: timeout\n", cmd); return 2; }
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        if (r < 0) { printf("read: %s\n", strerror(errno)); return 1; }
        buf[r] = 0;
        printf("%s: readable, got %s", cmd, buf);
    } else {
        fprintf(stderr, "bad command\n");
        return 1;
    }
    return 0;
}
