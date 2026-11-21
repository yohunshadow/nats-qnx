/*
 * devnats.h - public devctl() API of the devnats QNX resource manager.
 *
 * devnats exposes NATS subjects as files under /dev/nats:
 *   /dev/nats/a/b/c      <->  subject "a.b.c"
 * write() publishes, read() returns one received message per call.
 * The devctl() commands below add the things a plain file can't express.
 *
 * Copyright 2026 nats-qnx authors. Licensed under the Apache License 2.0.
 */
#ifndef DEVNATS_H_
#define DEVNATS_H_

#include <stdint.h>
#include <devctl.h>

#define DEVNATS_DEFAULT_MOUNT   "/dev/nats"
#define DEVNATS_MAX_SUBJECT     256
#define DEVNATS_MAX_PAYLOAD     4000    /* payload carried inside one devctl */

/* Global + per-open (per-OCB) counters. */
typedef struct devnats_stats {
    /* whole daemon */
    uint64_t    msgs_in;        /* messages received from NATS (all opens) */
    uint64_t    msgs_out;       /* messages published to NATS */
    uint64_t    bytes_in;
    uint64_t    bytes_out;
    uint64_t    drops;          /* messages dropped because a queue was full */
    uint32_t    opens;          /* currently open OCBs */
    uint32_t    connected;      /* 1 if the NATS connection is up */
    uint32_t    reconnects;
    uint32_t    reserved;
    char        server[128];    /* currently connected server URL */
    /* this open file descriptor */
    uint64_t    ocb_msgs_in;
    uint64_t    ocb_msgs_out;
    uint64_t    ocb_drops;
    uint32_t    ocb_queued;     /* messages waiting to be read */
    uint32_t    ocb_qmax;       /* queue bound (oldest dropped when full) */
    char        subject[DEVNATS_MAX_SUBJECT];
    char        queue_group[64];
} devnats_stats_t;

/* Request/reply on the fd's subject. In: timeout_ms, len, data. Out: len, data. */
typedef struct devnats_request {
    uint32_t    timeout_ms;
    uint32_t    len;
    int32_t     status;         /* natsStatus of the request (0 = OK); on failure
                                   devctl() itself returns ETIMEDOUT, ENOENT
                                   (no responders) or EIO */
    uint32_t    reserved;
    char        data[DEVNATS_MAX_PAYLOAD];
} devnats_request_t;

/* JetStream publish with ack on the fd's subject. */
typedef struct devnats_jspub {
    uint32_t    len;
    int32_t     status;         /* natsStatus (0 = OK); on failure devctl()
                                   returns ENOENT (no stream), ETIMEDOUT or EIO */
    int32_t     js_err;         /* jsErrCode from the server, if any */
    uint32_t    duplicate;
    uint64_t    seq;            /* stream sequence from the PubAck */
    char        stream[64];
    char        msg_id[64];     /* optional Nats-Msg-Id for de-duplication */
    char        data[DEVNATS_MAX_PAYLOAD];
} devnats_jspub_t;

/* Metadata of the last message returned by read() on this fd. */
typedef struct devnats_msginfo {
    uint64_t    enqueue_ns;     /* CLOCK_MONOTONIC when devnats got it from NATS */
    uint64_t    deliver_ns;     /* CLOCK_MONOTONIC when it was handed to read() */
    uint64_t    seq;            /* per-fd sequence number of received messages */
    uint32_t    len;
    uint32_t    reserved;
    char        subject[DEVNATS_MAX_SUBJECT];
    char        reply[DEVNATS_MAX_SUBJECT];
} devnats_msginfo_t;

#define DEVNATS_CMD_BASE        _DCMD_MISC
#define DCMD_NATS_GET_STATS     __DIOF(DEVNATS_CMD_BASE, 0x41, devnats_stats_t)
#define DCMD_NATS_SET_QMAX      __DIOT(DEVNATS_CMD_BASE, 0x42, uint32_t)
#define DCMD_NATS_SET_RAW       __DIOT(DEVNATS_CMD_BASE, 0x43, uint32_t)
#define DCMD_NATS_SET_QGROUP    __DIOT(DEVNATS_CMD_BASE, 0x44, char[64])
#define DCMD_NATS_REQUEST       __DIOTF(DEVNATS_CMD_BASE, 0x45, devnats_request_t)
#define DCMD_NATS_JS_PUBLISH    __DIOTF(DEVNATS_CMD_BASE, 0x46, devnats_jspub_t)
#define DCMD_NATS_LAST_MSGINFO  __DIOF(DEVNATS_CMD_BASE, 0x47, devnats_msginfo_t)
#define DCMD_NATS_FLUSH         __DION(DEVNATS_CMD_BASE, 0x48)

#endif /* DEVNATS_H_ */
