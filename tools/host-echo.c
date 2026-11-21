/*
 * host-echo: tiny NATS helper that runs on the Linux dev host for the
 * devnats tests and latency runs.
 *   lat.ping  -> republished unchanged on lat.pong   (round-trip probe)
 *   svc.echo  -> request/reply: replies "echo: <payload>"
 *   svc.time  -> request/reply: replies the host's UTC time
 * usage: host-echo [nats://127.0.0.1:4222]
 * Copyright 2026 nats-qnx authors. Licensed under the Apache License 2.0.
 */
#include <nats/nats.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

static void on_ping(natsConnection *nc, natsSubscription *s, natsMsg *m, void *c)
{
    natsConnection_Publish(nc, "lat.pong", natsMsg_GetData(m), natsMsg_GetDataLength(m));
    natsMsg_Destroy(m);
}

static void on_echo(natsConnection *nc, natsSubscription *s, natsMsg *m, void *c)
{
    char buf[4200];
    int n = snprintf(buf, sizeof(buf), "echo: %.*s", natsMsg_GetDataLength(m), natsMsg_GetData(m));
    if (natsMsg_GetReply(m)) natsConnection_Publish(nc, natsMsg_GetReply(m), buf, n);
    natsMsg_Destroy(m);
}

static void on_time(natsConnection *nc, natsSubscription *s, natsMsg *m, void *c)
{
    char buf[64];
    time_t t = time(NULL);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ (linux host)", gmtime(&t));
    if (natsMsg_GetReply(m)) natsConnection_PublishString(nc, natsMsg_GetReply(m), buf);
    natsMsg_Destroy(m);
}

int main(int argc, char **argv)
{
    natsConnection *nc = NULL;
    natsSubscription *a = NULL, *b = NULL, *d = NULL;
    const char *url = argc > 1 ? argv[1] : "nats://127.0.0.1:4222";
    if (natsConnection_ConnectTo(&nc, url) != NATS_OK) { fprintf(stderr, "connect failed\n"); return 1; }
    natsConnection_Subscribe(&a, nc, "lat.ping", on_ping, NULL);
    natsConnection_Subscribe(&b, nc, "svc.echo", on_echo, NULL);
    natsConnection_Subscribe(&d, nc, "svc.time", on_time, NULL);
    natsConnection_Flush(nc);
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig);
    printf("host-echo ready on %s (lat.ping->lat.pong, svc.echo, svc.time)\n", url);
    fflush(stdout);
    while (!stop) pause();
    natsSubscription_Destroy(a); natsSubscription_Destroy(b); natsSubscription_Destroy(d);
    natsConnection_Destroy(nc);
    return 0;
}
