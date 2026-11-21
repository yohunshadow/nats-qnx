/*
 * nats-notify: event-driven /dev/nats subscriber using ionotify() + a pulse.
 *
 * The thread blocks in MsgReceivePulse() and costs no CPU until devnats
 * delivers our (registered) pulse with MsgDeliverEvent() because a message
 * was queued on the fd.  Then it drains the fd with non-blocking read()s and
 * re-arms.  Optionally runs SCHED_FIFO at a given priority.
 *
 * usage: nats-notify [-p prio] [-n count] [-q] /dev/nats/<subject path>
 *
 * Copyright 2026 nats-qnx authors. Licensed under the Apache License 2.0.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <time.h>
#include <unistd.h>
#include <devctl.h>
#include "../devnats.h"

#define PULSE_CODE_NATS (_PULSE_CODE_MINAVAIL + 7)

int main(int argc, char **argv)
{
    int prio = 0, count = -1, quiet = 0, opt, got = 0, chid, coid, fd;
    struct sigevent ev;
    char buf[4096];

    while ((opt = getopt(argc, argv, "p:n:q")) != -1) {
        switch (opt) {
        case 'p': prio = atoi(optarg); break;
        case 'n': count = atoi(optarg); break;
        case 'q': quiet = 1; break;
        default: goto usage;
        }
    }
    if (optind >= argc) {
usage:
        fprintf(stderr, "usage: %s [-p fifo_prio] [-n count] [-q] /dev/nats/<path>\n", argv[0]);
        return 1;
    }
    if (prio > 0) {
        struct sched_param sp = { .sched_priority = prio };
        int r = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (r != EOK)
            fprintf(stderr, "SCHED_FIFO %d: %s (continuing)\n", prio, strerror(r));
    }
    if ((fd = open(argv[optind], O_RDONLY | O_NONBLOCK)) == -1) {
        perror(argv[optind]);
        return 1;
    }
    chid = ChannelCreate(_NTO_CHF_PRIVATE);
    coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    SIGEV_PULSE_INIT(&ev, coid, prio > 0 ? prio : SIGEV_PULSE_PRIO_INHERIT, PULSE_CODE_NATS, 0);
    /* QNX 7.1+: register the event so only this server (fd) may deliver it */
    if (MsgRegisterEvent(&ev, fd) == -1) {
        perror("MsgRegisterEvent");
        return 1;
    }
    printf("nats-notify: waiting on %s (pulse, %s prio %d)\n", argv[optind],
           prio > 0 ? "SCHED_FIFO" : "inherited", prio);
    fflush(stdout);

    while (count < 0 || got < count) {
        /* arm; if data is already there ionotify says so and we skip the wait */
        int r = ionotify(fd, _NOTIFY_ACTION_POLLARM, _NOTIFY_COND_INPUT, &ev);
        if (r == -1) {
            perror("ionotify");
            return 1;
        }
        if (!(r & _NOTIFY_COND_INPUT)) {
            struct _pulse p;
            if (MsgReceivePulse(chid, &p, sizeof(p), NULL) == -1) {
                perror("MsgReceivePulse");
                return 1;
            }
            if (p.code != PULSE_CODE_NATS)
                continue;
        }
        for (;;) {                                  /* drain */
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n == -1 && errno == EAGAIN)
                break;
            if (n <= 0) {
                if (n == 0) printf("nats-notify: EOF (devnats stopped)\n");
                else perror("read");
                return n == 0 ? 0 : 1;
            }
            buf[n] = '\0';
            got++;
            if (!quiet) {
                devnats_msginfo_t mi;
                if (devctl(fd, DCMD_NATS_LAST_MSGINFO, &mi, sizeof(mi), NULL) == EOK)
                    printf("[pulse] #%d %s: %s", got, mi.subject, buf);
                else
                    printf("[pulse] #%d %s", got, buf);
                if (n > 0 && buf[n - 1] != '\n') putchar('\n');
                fflush(stdout);
            }
            if (count >= 0 && got >= count)
                break;
        }
    }
    MsgUnregisterEvent(&ev);
    close(fd);
    printf("nats-notify: received %d message(s)\n", got);
    return 0;
}
