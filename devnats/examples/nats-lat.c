/*
 * nats-lat: wake-up latency of a /dev/nats reader, ionotify-pulse vs polling.
 *
 * A writer thread writes N messages (payload = CLOCK_MONOTONIC send time) to
 * the write path every interval.  The main thread reads the read path either
 *   -m pulse : ionotify(_NOTIFY_COND_INPUT) + MsgReceivePulse, SCHED_FIFO
 *   -m poll  : non-blocking read() + nanosleep(poll interval) loop
 *   -m block : plain blocking read()
 * and reports, per message,
 *   wake  = time the reader got the data   - time devnats queued it
 *           (pure resource-manager -> client wake-up, same clock)
 *   e2e   = time the reader got the data   - time the writer wrote it
 *           (write -> devnats -> NATS server -> devnats -> reader)
 * With -w lat.ping -r lat.pong and a host echo service the e2e number is the
 * guest -> host -> guest round trip.
 *
 * Copyright 2026 nats-qnx authors. Licensed under the Apache License 2.0.
 */
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>
#include "../devnats.h"

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *wpath;
static int nmsg = 200, interval_us = 10000, wprio = 10;

static void *writer(void *a)
{
    int fd = open(wpath, O_WRONLY);
    char buf[64];
    (void)a;
    if (fd == -1) { perror(wpath); exit(1); }
    usleep(300000);
    for (int i = 0; i < nmsg; i++) {
        int n = snprintf(buf, sizeof(buf), "%d %" PRIu64 "\n", i, now_ns());
        if (write(fd, buf, (size_t)n) != n) { perror("write"); exit(1); }
        usleep((useconds_t)interval_us);
    }
    close(fd);
    return NULL;
}

static int cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void report(const char *name, uint64_t *v, int n)
{
    double sum = 0;
    if (n == 0) { printf("%-6s n=0\n", name); return; }
    qsort(v, (size_t)n, sizeof(*v), cmp);
    for (int i = 0; i < n; i++) sum += (double)v[i];
    printf("%-5s n=%-4d min=%7.1f  p50=%7.1f  avg=%7.1f  p90=%7.1f  p99=%7.1f  max=%8.1f us\n",
           name, n, v[0] / 1e3, v[n / 2] / 1e3, sum / n / 1e3, v[(n * 9) / 10] / 1e3,
           v[(n * 99) / 100] / 1e3, v[n - 1] / 1e3);
}

int main(int argc, char **argv)
{
    const char *mode = "pulse", *rpath = NULL;
    int prio = 20, poll_us = 1000, opt, got = 0, fd, chid = -1;
    struct sigevent ev;
    pthread_t wt;
    pthread_attr_t wa;
    struct sched_param sp;
    uint64_t *wake, *e2e;
    char buf[256];

    while ((opt = getopt(argc, argv, "m:w:r:n:i:p:P:")) != -1) {
        switch (opt) {
        case 'm': mode = optarg; break;
        case 'w': wpath = optarg; break;
        case 'r': rpath = optarg; break;
        case 'n': nmsg = atoi(optarg); break;
        case 'i': interval_us = atoi(optarg); break;
        case 'p': prio = atoi(optarg); break;
        case 'P': poll_us = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: nats-lat [-m pulse|poll|block] -w /dev/nats/x -r /dev/nats/y "
                            "[-n count] [-i interval_us] [-p fifo_prio] [-P poll_us]\n");
            return 1;
        }
    }
    if (!wpath) wpath = "/dev/nats/lat/local";
    if (!rpath) rpath = wpath;
    wake = calloc((size_t)nmsg, sizeof(*wake));
    e2e = calloc((size_t)nmsg, sizeof(*e2e));

    sp.sched_priority = prio;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    fd = open(rpath, O_RDONLY | (strcmp(mode, "block") ? O_NONBLOCK : 0));
    if (fd == -1) { perror(rpath); return 1; }
    if (strcmp(mode, "pulse") == 0) {
        int coid;
        chid = ChannelCreate(_NTO_CHF_PRIVATE);
        coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
        SIGEV_PULSE_INIT(&ev, coid, prio, _PULSE_CODE_MINAVAIL, 0);
        if (MsgRegisterEvent(&ev, fd) == -1) { perror("MsgRegisterEvent"); return 1; }
    }

    pthread_attr_init(&wa);
    pthread_attr_setinheritsched(&wa, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&wa, SCHED_FIFO);
    sp.sched_priority = wprio;
    pthread_attr_setschedparam(&wa, &sp);
    pthread_create(&wt, &wa, writer, NULL);

    uint64_t deadline = now_ns() + (uint64_t)nmsg * (uint64_t)interval_us * 1000ull + 10000000000ull;
    while (got < nmsg && now_ns() < deadline) {
        ssize_t n;
        uint64_t t;
        if (strcmp(mode, "pulse") == 0) {
            int r = ionotify(fd, _NOTIFY_ACTION_POLLARM, _NOTIFY_COND_INPUT, &ev);
            if (r == -1) { perror("ionotify"); return 1; }
            if (!(r & _NOTIFY_COND_INPUT)) {
                struct _pulse p;
                uint64_t to = 2000000000ull;
                TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &to, NULL);
                if (MsgReceivePulse(chid, &p, sizeof(p), NULL) == -1)
                    continue;
            }
        }
        for (;;) {
            n = read(fd, buf, sizeof(buf) - 1);
            t = now_ns();
            if (n == -1 && errno == EAGAIN) {
                if (strcmp(mode, "poll") == 0 && t < deadline) {
                    struct timespec ts = { 0, (long)poll_us * 1000L };
                    nanosleep(&ts, NULL);
                    continue;
                }
                break;
            }
            if (n <= 0) { perror("read"); return 1; }
            buf[n] = '\0';
            {
                devnats_msginfo_t mi;
                int seq;
                uint64_t sent;
                if (sscanf(buf, "%d %" SCNu64, &seq, &sent) == 2 &&
                    devctl(fd, DCMD_NATS_LAST_MSGINFO, &mi, sizeof(mi), NULL) == EOK &&
                    got < nmsg) {
                    wake[got] = t - mi.enqueue_ns;
                    e2e[got] = t - sent;
                    got++;
                }
            }
            if (strcmp(mode, "pulse") != 0)
                break;                         /* poll/block: one per loop */
        }
    }
    pthread_join(wt, NULL);
    printf("mode=%s  read=%s  write=%s  msgs=%d/%d  interval=%dus  reader SCHED_FIFO %d%s\n",
           mode, rpath, wpath, got, nmsg, interval_us, prio,
           strcmp(mode, "poll") == 0 ? "  (poll sleep 1 tick)" : "");
    report("wake", wake, got);
    report("e2e", e2e, got);
    return got == nmsg ? 0 : 2;
}
