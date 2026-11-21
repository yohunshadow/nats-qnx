/*
 * devnats - NATS as a QNX resource manager.
 *
 *   /dev/nats/<a>/<b>/<c>   <->   NATS subject "a.b.c"
 *
 *   write()   publishes one message (text mode: one message per line)
 *   read()    returns one received message per call; blocks unless O_NONBLOCK
 *   ionotify()/select()/poll()  _NOTIFY_COND_INPUT when a message is queued
 *   devctl()  stats, queue bound, raw mode, queue group, request/reply,
 *             JetStream publish with ack, last-message metadata, flush
 *
 * Each open() gets its own OCB with a bounded message queue (oldest message
 * dropped when full, per-OCB and global drop counters).  Messages arrive on
 * nats.c delivery threads and are either handed straight to a blocked
 * reader (MsgReply) or queued and announced with iofunc_notify_trigger()
 * (MsgDeliverEvent of the client's pulse/signal).
 *
 * Copyright 2026 nats-qnx authors. Licensed under the Apache License 2.0.
 */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct dn_ocb;
struct dn_attr;
#define IOFUNC_OCB_T        struct dn_ocb
#define IOFUNC_ATTR_T       struct dn_attr
#define RESMGR_OCB_T        struct dn_ocb
#define RESMGR_HANDLE_T     struct dn_attr
#define THREAD_POOL_PARAM_T dispatch_context_t
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <sys/slog2.h>

#include <nats/nats.h>
#include "devnats.h"

#define DN_MAX_WAITERS  16
#define DN_DEFAULT_QMAX 256

/* ------------------------------------------------------------------ types */

typedef struct dn_msg {
    struct dn_msg   *next;
    uint64_t        enq_ns;
    int             len;
    char            subject[DEVNATS_MAX_SUBJECT];
    char            reply[DEVNATS_MAX_SUBJECT];
    char            data[];
} dn_msg_t;

typedef struct dn_waiter {
    rcvid_t         rcvid;
    size_t          nbytes;
} dn_waiter_t;

struct dn_attr {
    iofunc_attr_t   hdr;                        /* must be first */
    bool            is_root;
    char            subject[DEVNATS_MAX_SUBJECT];
    struct dn_ocb   *ocb;                       /* per-open attr -> its OCB */
};

struct dn_ocb {
    iofunc_ocb_t        hdr;                    /* must be first */
    pthread_mutex_t     mu;                     /* queue, waiters, notify, stats */
    pthread_cond_t      cv;
    struct dn_ocb       *gnext, *gprev;         /* global list (shutdown) */
    char                subject[DEVNATS_MAX_SUBJECT];
    bool                wildcard;
    bool                raw;
    char                qgroup[64];
    natsSubscription    *sub;
    bool                sub_done;
    dn_msg_t            *head, *tail;
    uint32_t            count, qmax;
    uint64_t            in, out, drops, rseq;
    dn_waiter_t         waiters[DN_MAX_WAITERS];
    int                 nwaiters;
    iofunc_notify_t     notify[3];
    devnats_msginfo_t   last;
};

/* ---------------------------------------------------------------- globals */

static natsConnection       *g_nc;
static resmgr_connect_funcs_t g_cfuncs;
static resmgr_io_funcs_t    g_iofuncs;
static iofunc_funcs_t       g_ocbfuncs;
static iofunc_mount_t       g_mount;
static struct dn_attr       g_root;
static dispatch_t           *g_dpp;
static int                  g_rid = -1;
static atomic_uint_fast64_t g_in, g_out, g_bin, g_bout, g_drops;
static atomic_uint          g_opens, g_connected, g_reconnects;
static atomic_uint          g_ino = 2;
static pthread_mutex_t      g_list_mu = PTHREAD_MUTEX_INITIALIZER;
static struct dn_ocb        *g_list;
static uint32_t             g_qmax = DN_DEFAULT_QMAX;
static bool                 g_raw_default;
static int                  g_verbose;
static volatile bool        g_shutting_down;
static slog2_buffer_t       g_slog;
static bool                 g_slog_ok;

/* ---------------------------------------------------------------- logging */

static void dn_log(int sev, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_slog_ok)
        slog2c(g_slog, 0, (uint8_t)sev, buf);
    if (g_verbose || sev <= SLOG2_ERROR)
        fprintf(stderr, "devnats: %s\n", buf);
}
#define LOGI(...) dn_log(SLOG2_INFO, __VA_ARGS__)
#define LOGW(...) dn_log(SLOG2_WARNING, __VA_ARGS__)
#define LOGE(...) dn_log(SLOG2_ERROR, __VA_ARGS__)
#define LOGD(...) do { if (g_verbose > 1) dn_log(SLOG2_DEBUG1, __VA_ARGS__); } while (0)

static void dn_slog_init(void)
{
    slog2_buffer_set_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.num_buffers = 1;
    cfg.buffer_set_name = "devnats";
    cfg.verbosity_level = SLOG2_DEBUG1;
    cfg.buffer_config[0].buffer_name = "devnats";
    cfg.buffer_config[0].num_pages = 4;
    g_slog_ok = (slog2_register(&cfg, &g_slog, 0) == 0);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* -------------------------------------------------------- subject mapping */

/*
 * "a/b/c" -> "a.b.c".  A component may itself contain dots
 * ("/dev/nats/a.b/c" is also "a.b.c").  "*" and ">" are NATS wildcards and
 * are only allowed for reading; ">" must be the last token.
 * Rejects empty tokens, whitespace and over-long subjects.
 */
static int path_to_subject(const char *path, char *subj, size_t len, bool *wild)
{
    size_t n = 0;
    const char *p;
    *wild = false;
    if (path == NULL || *path == '\0')
        return EINVAL;
    for (p = path; *p; p++) {
        char c = (*p == '/') ? '.' : *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            return EINVAL;
        if (n + 1 >= len)
            return ENAMETOOLONG;
        subj[n++] = c;
    }
    subj[n] = '\0';
    /* validate tokens */
    {
        char tmp[DEVNATS_MAX_SUBJECT];
        char *save = NULL, *tok;
        size_t ntok = 0, last_gt = 0;
        if (subj[0] == '.' || subj[n - 1] == '.' || strstr(subj, "..") != NULL)
            return EINVAL;
        strcpy(tmp, subj);
        for (tok = strtok_r(tmp, ".", &save); tok; tok = strtok_r(NULL, ".", &save)) {
            ntok++;
            if (strcmp(tok, "*") == 0)
                *wild = true;
            else if (strcmp(tok, ">") == 0) {
                *wild = true;
                last_gt = ntok;
            } else if (strchr(tok, '*') || strchr(tok, '>'))
                return EINVAL;
        }
        if (last_gt && last_gt != ntok)
            return EINVAL;
    }
    return EOK;
}

/* --------------------------------------------------------- message queue */

static void q_push(struct dn_ocb *o, dn_msg_t *m)
{
    m->next = NULL;
    if (o->tail) o->tail->next = m; else o->head = m;
    o->tail = m;
    o->count++;
}

static dn_msg_t *q_pop(struct dn_ocb *o)
{
    dn_msg_t *m = o->head;
    if (m) {
        o->head = m->next;
        if (o->head == NULL) o->tail = NULL;
        o->count--;
    }
    return m;
}

/* Reply one message to a blocked/ready reader.  Called with o->mu held. */
static void deliver(struct dn_ocb *o, rcvid_t rcvid, size_t nbytes, dn_msg_t *m)
{
    iov_t iov[2];
    int parts = 1;
    size_t len = (size_t)m->len;
    bool add_nl = !o->raw && (len == 0 || m->data[len - 1] != '\n');

    if (len > nbytes) len = nbytes;           /* truncated: rest discarded */
    SETIOV(&iov[0], m->data, len);
    if (add_nl && len < nbytes) {
        SETIOV(&iov[1], "\n", 1);
        parts = 2;
        len++;
    }
    o->rseq++;
    o->last.enqueue_ns = m->enq_ns;
    o->last.deliver_ns = now_ns();
    o->last.seq = o->rseq;
    o->last.len = (uint32_t)m->len;
    strcpy(o->last.subject, m->subject);
    strcpy(o->last.reply, m->reply);
    MsgReplyv(rcvid, (long)len, iov, parts);
}

static void on_msg(natsConnection *nc, natsSubscription *sub, natsMsg *nm, void *closure)
{
    struct dn_ocb *o = closure;
    int len = natsMsg_GetDataLength(nm);
    dn_msg_t *m = malloc(sizeof(*m) + (size_t)len);

    (void)nc; (void)sub;
    if (m == NULL) {
        natsMsg_Destroy(nm);
        return;
    }
    m->enq_ns = now_ns();
    m->len = len;
    memcpy(m->data, natsMsg_GetData(nm), (size_t)len);
    snprintf(m->subject, sizeof(m->subject), "%s", natsMsg_GetSubject(nm));
    snprintf(m->reply, sizeof(m->reply), "%s",
             natsMsg_GetReply(nm) ? natsMsg_GetReply(nm) : "");
    natsMsg_Destroy(nm);

    atomic_fetch_add(&g_in, 1);
    atomic_fetch_add(&g_bin, (uint64_t)len);

    pthread_mutex_lock(&o->mu);
    o->in++;
    if (o->nwaiters > 0) {
        dn_waiter_t w = o->waiters[0];
        memmove(&o->waiters[0], &o->waiters[1], (size_t)(o->nwaiters - 1) * sizeof(w));
        o->nwaiters--;
        deliver(o, w.rcvid, w.nbytes, m);
        free(m);
    } else {
        if (o->count >= o->qmax) {
            free(q_pop(o));                   /* drop oldest */
            o->drops++;
            atomic_fetch_add(&g_drops, 1);
        }
        q_push(o, m);
        if (IOFUNC_NOTIFY_INPUT_CHECK(o->notify, (int)o->count, 0))
            iofunc_notify_trigger(o->notify, (int)o->count, IOFUNC_NOTIFY_INPUT);
    }
    pthread_mutex_unlock(&o->mu);
}

static void on_sub_complete(void *closure)
{
    struct dn_ocb *o = closure;
    pthread_mutex_lock(&o->mu);
    o->sub_done = true;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->mu);
}

/* Must be called WITHOUT o->mu held (waits for in-flight callbacks). */
static void sub_stop(struct dn_ocb *o)
{
    natsSubscription *s;
    pthread_mutex_lock(&o->mu);
    s = o->sub;
    o->sub = NULL;
    pthread_mutex_unlock(&o->mu);
    if (s == NULL)
        return;
    if (natsSubscription_Unsubscribe(s) == NATS_OK) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += 5;
        pthread_mutex_lock(&o->mu);
        while (!o->sub_done)
            if (pthread_cond_timedwait(&o->cv, &o->mu, &ts) == ETIMEDOUT)
                break;
        pthread_mutex_unlock(&o->mu);
    }
    natsSubscription_Destroy(s);
}

static natsStatus sub_start(struct dn_ocb *o)
{
    natsSubscription *s = NULL;
    natsStatus st;
    o->sub_done = false;
    if (o->qgroup[0])
        st = natsConnection_QueueSubscribe(&s, g_nc, o->subject, o->qgroup, on_msg, o);
    else
        st = natsConnection_Subscribe(&s, g_nc, o->subject, on_msg, o);
    if (st == NATS_OK) {
        natsSubscription_SetOnCompleteCB(s, on_sub_complete, o);
        /* the per-OCB queue is our bound; let the library buffer generously */
        natsSubscription_SetPendingLimits(s, 65536, 64 * 1024 * 1024);
        pthread_mutex_lock(&o->mu);
        o->sub = s;
        pthread_mutex_unlock(&o->mu);
    }
    return st;
}

/* --------------------------------------------------------- OCB lifecycle */

static struct dn_ocb *dn_ocb_calloc(resmgr_context_t *ctp, struct dn_attr *attr)
{
    struct dn_ocb *o = calloc(1, sizeof(*o));
    pthread_condattr_t ca;
    (void)ctp;
    if (o == NULL)
        return NULL;
    pthread_mutex_init(&o->mu, NULL);
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&o->cv, &ca);
    pthread_condattr_destroy(&ca);
    IOFUNC_NOTIFY_INIT(o->notify);
    o->qmax = g_qmax;
    o->raw = g_raw_default;
    if (!attr->is_root) {
        strcpy(o->subject, attr->subject);
        attr->ocb = o;
    }
    pthread_mutex_lock(&g_list_mu);
    o->gnext = g_list;
    if (g_list) g_list->gprev = o;
    g_list = o;
    pthread_mutex_unlock(&g_list_mu);
    atomic_fetch_add(&g_opens, 1);
    return o;
}

static void dn_ocb_free(struct dn_ocb *o)
{
    struct dn_attr *attr = o->hdr.attr;
    dn_msg_t *m;

    sub_stop(o);
    pthread_mutex_lock(&g_list_mu);
    if (o->gprev) o->gprev->gnext = o->gnext; else g_list = o->gnext;
    if (o->gnext) o->gnext->gprev = o->gprev;
    pthread_mutex_unlock(&g_list_mu);
    pthread_mutex_lock(&o->mu);
    while ((m = q_pop(o)) != NULL)
        free(m);
    for (int i = 0; i < o->nwaiters; i++)          /* should not happen */
        MsgError(o->waiters[i].rcvid, EBADF);
    o->nwaiters = 0;
    pthread_mutex_unlock(&o->mu);
    pthread_mutex_destroy(&o->mu);
    pthread_cond_destroy(&o->cv);
    atomic_fetch_sub(&g_opens, 1);
    free(o);
    if (attr != NULL && !attr->is_root)
        free(attr);                                 /* attrs are per-open */
}

/* ------------------------------------------------------------- handlers */

static int dn_open(resmgr_context_t *ctp, io_open_t *msg, struct dn_attr *handle, void *extra)
{
    struct dn_attr *a;
    bool wild = false;
    int rc;
    char subj[DEVNATS_MAX_SUBJECT];

    if (msg->connect.path[0] == '\0')
        return iofunc_open_default(ctp, msg, &g_root.hdr, extra);

    if ((rc = path_to_subject(msg->connect.path, subj, sizeof(subj), &wild)) != EOK)
        return rc;
    if (g_shutting_down)
        return ENXIO;
    a = calloc(1, sizeof(*a));
    if (a == NULL)
        return ENOMEM;
    iofunc_attr_init(&a->hdr, S_IFCHR | 0666, NULL, NULL);
    a->hdr.mount = &g_mount;
    a->hdr.inode = atomic_fetch_add(&g_ino, 1);
    strcpy(a->subject, subj);

    rc = iofunc_open_default(ctp, msg, &a->hdr, extra);
    if (rc != EOK) {
        free(a);
        return rc;
    }
    {
        struct dn_ocb *o = a->ocb;
        o->wildcard = wild;
        if (msg->connect.ioflag & _IO_FLAG_RD) {
            natsStatus st = sub_start(o);
            if (st != NATS_OK) {
                LOGE("subscribe '%s' failed: %s", subj, natsStatus_GetText(st));
                /* The OCB is bound; the client sees EIO on read. */
            } else {
                LOGD("open %s (subscribed)", subj);
            }
        } else {
            LOGD("open %s (write-only)", subj);
        }
    }
    return EOK;
}

static int dn_read(resmgr_context_t *ctp, io_read_t *msg, struct dn_ocb *o)
{
    int nonblock = 0, rc;
    dn_msg_t *m;

    if ((rc = iofunc_read_verify(ctp, msg, &o->hdr, &nonblock)) != EOK)
        return rc;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    if (o->hdr.attr->is_root) {              /* "ls /dev/nats": empty dir */
        _IO_SET_READ_NBYTES(ctp, 0);
        return _RESMGR_NPARTS(0);
    }
    pthread_mutex_lock(&o->mu);
    if (o->sub == NULL) {
        pthread_mutex_unlock(&o->mu);
        return EIO;
    }
    if ((m = q_pop(o)) != NULL) {
        deliver(o, ctp->rcvid, msg->i.nbytes, m);
        pthread_mutex_unlock(&o->mu);
        free(m);
        return _RESMGR_NOREPLY;
    }
    if (nonblock || g_shutting_down) {
        pthread_mutex_unlock(&o->mu);
        if (g_shutting_down) {
            _IO_SET_READ_NBYTES(ctp, 0);
            return _RESMGR_NPARTS(0);
        }
        return EAGAIN;
    }
    if (o->nwaiters >= DN_MAX_WAITERS) {
        pthread_mutex_unlock(&o->mu);
        return EBUSY;
    }
    o->waiters[o->nwaiters].rcvid = ctp->rcvid;
    o->waiters[o->nwaiters].nbytes = msg->i.nbytes;
    o->nwaiters++;
    pthread_mutex_unlock(&o->mu);
    return _RESMGR_NOREPLY;
}

static int publish_one(struct dn_ocb *o, const char *data, int len)
{
    natsStatus st = natsConnection_Publish(g_nc, o->subject, data, len);
    if (st != NATS_OK)
        return -1;
    atomic_fetch_add(&g_out, 1);
    atomic_fetch_add(&g_bout, (uint64_t)len);
    o->out++;
    return 0;
}

static int dn_write(resmgr_context_t *ctp, io_write_t *msg, struct dn_ocb *o)
{
    int rc;
    size_t nbytes = (size_t)_IO_WRITE_GET_NBYTES(msg);
    char *buf;

    if ((rc = iofunc_write_verify(ctp, msg, &o->hdr, NULL)) != EOK)
        return rc;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    if (o->hdr.attr->is_root)
        return EISDIR;
    if (o->wildcard)
        return EINVAL;                      /* cannot publish to a wildcard */
    if ((buf = malloc(nbytes + 1)) == NULL)
        return ENOMEM;
    if (resmgr_msgget(ctp, buf, nbytes, sizeof(msg->i)) != (ssize_t)nbytes) {
        free(buf);
        return EIO;
    }
    if (o->raw) {
        rc = publish_one(o, buf, (int)nbytes);
    } else {
        /* text mode: one message per line, newline stripped */
        size_t start = 0;
        rc = 0;
        for (size_t i = 0; i <= nbytes && rc == 0; i++) {
            if (i == nbytes || buf[i] == '\n') {
                if (i > start || (i < nbytes && nbytes == 1))
                    rc = publish_one(o, buf + start, (int)(i - start));
                start = i + 1;
            }
        }
    }
    free(buf);
    if (rc != 0)
        return EIO;
    _IO_SET_WRITE_NBYTES(ctp, (long)nbytes);
    if (nbytes > 0)
        o->hdr.attr->hdr.flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;
    return _RESMGR_NPARTS(0);
}

static int dn_notify(resmgr_context_t *ctp, io_notify_t *msg, struct dn_ocb *o)
{
    int trig, rc;
    pthread_mutex_lock(&o->mu);
    trig = _NOTIFY_COND_OUTPUT;
    if (o->count > 0)
        trig |= _NOTIFY_COND_INPUT;
    rc = iofunc_notify(ctp, msg, o->notify, trig, NULL, NULL);
    pthread_mutex_unlock(&o->mu);
    return rc;
}

static int dn_unblock(resmgr_context_t *ctp, io_pulse_t *msg, struct dn_ocb *o)
{
    pthread_mutex_lock(&o->mu);
    for (int i = 0; i < o->nwaiters; i++) {
        if (o->waiters[i].rcvid == ctp->rcvid) {
            struct _msg_info info;
            memmove(&o->waiters[i], &o->waiters[i + 1],
                    (size_t)(o->nwaiters - i - 1) * sizeof(o->waiters[0]));
            o->nwaiters--;
            pthread_mutex_unlock(&o->mu);
            /* Only unblock if the client really asked (signal/timeout). */
            if (MsgInfo(ctp->rcvid, &info) != -1 && (info.flags & _NTO_MI_UNBLOCK_REQ))
                MsgError(ctp->rcvid, EINTR);
            return _RESMGR_NOREPLY;
        }
    }
    pthread_mutex_unlock(&o->mu);
    return iofunc_unblock_default(ctp, msg, &o->hdr);
}

static int dn_close_ocb(resmgr_context_t *ctp, void *reserved, struct dn_ocb *o)
{
    pthread_mutex_lock(&o->mu);
    iofunc_notify_remove(ctp, o->notify);
    pthread_mutex_unlock(&o->mu);
    return iofunc_close_ocb_default(ctp, reserved, &o->hdr);
}

static int dn_devctl(resmgr_context_t *ctp, io_devctl_t *msg, struct dn_ocb *o)
{
    int rc, status = EOK;
    size_t out = 0;
    void *data = _DEVCTL_DATA(msg->i);
    size_t need = (size_t)(msg->i.dcmd >> 16) & 0x3fff;

    if ((rc = iofunc_devctl_default(ctp, msg, &o->hdr)) != (int)_RESMGR_DEFAULT)
        return rc;
    if (o->hdr.attr->is_root && msg->i.dcmd != DCMD_NATS_GET_STATS)
        return ENOTTY;
    if (sizeof(msg->i) + need > (size_t)ctp->msg_max_size)
        return E2BIG;
    if ((msg->i.dcmd & DEVDIR_TO) && (size_t)ctp->info.msglen < sizeof(msg->i) + need) {
        /* make sure the whole input payload is in our buffer */
        if (resmgr_msgread(ctp, data, need, sizeof(msg->i)) == -1)
            return errno;
    }

    switch ((unsigned)msg->i.dcmd) {
    case (unsigned)DCMD_NATS_GET_STATS: {
        devnats_stats_t *s = data;
        memset(s, 0, sizeof(*s));
        s->msgs_in = atomic_load(&g_in);
        s->msgs_out = atomic_load(&g_out);
        s->bytes_in = atomic_load(&g_bin);
        s->bytes_out = atomic_load(&g_bout);
        s->drops = atomic_load(&g_drops);
        s->opens = atomic_load(&g_opens);
        s->connected = natsConnection_Status(g_nc) == NATS_CONN_STATUS_CONNECTED;
        s->reconnects = atomic_load(&g_reconnects);
        natsConnection_GetConnectedUrl(g_nc, s->server, sizeof(s->server));
        pthread_mutex_lock(&o->mu);
        s->ocb_msgs_in = o->in;
        s->ocb_msgs_out = o->out;
        s->ocb_drops = o->drops;
        s->ocb_queued = o->count;
        s->ocb_qmax = o->qmax;
        snprintf(s->subject, sizeof(s->subject), "%s", o->subject);
        snprintf(s->queue_group, sizeof(s->queue_group), "%s", o->qgroup);
        pthread_mutex_unlock(&o->mu);
        out = sizeof(*s);
        break;
    }
    case (unsigned)DCMD_NATS_SET_QMAX: {
        uint32_t v = *(uint32_t *)data;
        if (v == 0 || v > 1000000)
            return EINVAL;
        pthread_mutex_lock(&o->mu);
        o->qmax = v;
        while (o->count > o->qmax) {
            free(q_pop(o));
            o->drops++;
            atomic_fetch_add(&g_drops, 1);
        }
        pthread_mutex_unlock(&o->mu);
        break;
    }
    case (unsigned)DCMD_NATS_SET_RAW:
        o->raw = (*(uint32_t *)data) != 0;
        break;
    case (unsigned)DCMD_NATS_SET_QGROUP: {
        char g[64];
        memcpy(g, data, sizeof(g));
        g[sizeof(g) - 1] = '\0';
        if (strpbrk(g, " \t\r\n") != NULL)
            return EINVAL;
        if (!(o->hdr.ioflag & _IO_FLAG_RD))
            return EBADF;
        sub_stop(o);
        snprintf(o->qgroup, sizeof(o->qgroup), "%s", g);
        if (sub_start(o) != NATS_OK)
            return EIO;
        LOGI("%s: queue group '%s'", o->subject, o->qgroup);
        break;
    }
    case (unsigned)DCMD_NATS_REQUEST: {
        devnats_request_t *r = data;
        natsMsg *reply = NULL;
        natsStatus st;
        if (o->wildcard || r->len > sizeof(r->data))
            return EINVAL;
        st = natsConnection_Request(&reply, g_nc, o->subject, r->data, (int)r->len,
                                    r->timeout_ms ? r->timeout_ms : 2000);
        r->status = (int32_t)st;
        r->len = 0;
        if (st == NATS_OK) {
            int n = natsMsg_GetDataLength(reply);
            if (n > (int)sizeof(r->data)) n = (int)sizeof(r->data);
            memcpy(r->data, natsMsg_GetData(reply), (size_t)n);
            r->len = (uint32_t)n;
            atomic_fetch_add(&g_out, 1);
            atomic_fetch_add(&g_in, 1);
            natsMsg_Destroy(reply);
        } else {
            status = (st == NATS_TIMEOUT) ? ETIMEDOUT
                   : (st == NATS_NO_RESPONDERS) ? ENOENT : EIO;
        }
        out = sizeof(*r);
        break;
    }
    case (unsigned)DCMD_NATS_JS_PUBLISH: {
        devnats_jspub_t *j = data;
        jsCtx *js = NULL;
        jsPubAck *pa = NULL;
        jsPubOptions po;
        jsErrCode jerr = 0;
        natsStatus st;
        if (o->wildcard || j->len > sizeof(j->data))
            return EINVAL;
        j->msg_id[sizeof(j->msg_id) - 1] = '\0';
        jsPubOptions_Init(&po);
        if (j->msg_id[0])
            po.MsgId = j->msg_id;
        st = natsConnection_JetStream(&js, g_nc, NULL);
        if (st == NATS_OK)
            st = js_Publish(&pa, js, o->subject, j->data, (int)j->len, &po, &jerr);
        j->status = (int32_t)st;
        j->js_err = (int32_t)jerr;
        if (st == NATS_OK && pa != NULL) {
            j->seq = pa->Sequence;
            j->duplicate = pa->Duplicate;
            snprintf(j->stream, sizeof(j->stream), "%s", pa->Stream);
            atomic_fetch_add(&g_out, 1);
            o->out++;
        } else {
            status = (st == NATS_NO_RESPONDERS) ? ENOENT
                   : (st == NATS_TIMEOUT) ? ETIMEDOUT : EIO;
        }
        jsPubAck_Destroy(pa);
        jsCtx_Destroy(js);
        out = sizeof(*j);
        break;
    }
    case (unsigned)DCMD_NATS_LAST_MSGINFO:
        pthread_mutex_lock(&o->mu);
        memcpy(data, &o->last, sizeof(o->last));
        pthread_mutex_unlock(&o->mu);
        out = sizeof(devnats_msginfo_t);
        break;
    case (unsigned)DCMD_NATS_FLUSH:
        if (natsConnection_FlushTimeout(g_nc, 2000) != NATS_OK)
            return EIO;
        break;
    default:
        return ENOTTY;
    }
    /* A failed request/JS publish is reported the conventional way: devctl()
     * returns the errno (ETIMEDOUT, ENOENT = no responders/no stream, EIO). */
    if (status != EOK)
        return status;
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.nbytes = (uint32_t)out;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + out);
}

/* -------------------------------------------------- connection callbacks */

static void on_disconnected(natsConnection *nc, void *c)
{
    (void)nc; (void)c;
    atomic_store(&g_connected, 0);
    LOGW("disconnected from NATS");
}

static void on_reconnected(natsConnection *nc, void *c)
{
    char url[128] = "";
    (void)c;
    atomic_store(&g_connected, 1);
    atomic_fetch_add(&g_reconnects, 1);
    natsConnection_GetConnectedUrl(nc, url, sizeof(url));
    LOGI("reconnected to %s", url);
}

static void on_connected(natsConnection *nc, void *c)
{
    char url[128] = "";
    (void)c;
    atomic_store(&g_connected, 1);
    natsConnection_GetConnectedUrl(nc, url, sizeof(url));
    LOGI("connected to %s", url);
}

/* ------------------------------------------------------ shutdown thread */

static void *signal_thread(void *arg)
{
    sigset_t *set = arg;
    int sig = 0;
    struct dn_ocb *o;

    sigwait(set, &sig);
    LOGI("signal %d: shutting down", sig);
    g_shutting_down = true;
    if (g_rid != -1)
        resmgr_detach(g_dpp, g_rid, _RESMGR_DETACH_PATHNAME);
    /* wake blocked readers with EOF so e.g. `cat` exits cleanly */
    pthread_mutex_lock(&g_list_mu);
    for (o = g_list; o; o = o->gnext) {
        pthread_mutex_lock(&o->mu);
        for (int i = 0; i < o->nwaiters; i++)
            MsgReply(o->waiters[i].rcvid, 0, NULL, 0);
        o->nwaiters = 0;
        iofunc_notify_trigger(o->notify, 1, IOFUNC_NOTIFY_INPUT);
        pthread_mutex_unlock(&o->mu);
    }
    pthread_mutex_unlock(&g_list_mu);
    if (g_nc) {
        natsConnection_FlushTimeout(g_nc, 1000);
        natsConnection_Close(g_nc);
    }
    LOGI("stopped: in=%llu out=%llu drops=%llu",
         (unsigned long long)atomic_load(&g_in), (unsigned long long)atomic_load(&g_out),
         (unsigned long long)atomic_load(&g_drops));
    _exit(0);
    return NULL;
}

/* ------------------------------------------------------------ privileges */

static void drop_privileges(const char *user)
{
    static const unsigned deny[] = {
        PROCMGR_AID_PATHSPACE, PROCMGR_AID_SPAWN_SETUID, PROCMGR_AID_SPAWN_SETGID,
        PROCMGR_AID_IO, PROCMGR_AID_INTERRUPT, PROCMGR_AID_MEM_PHYS,
        PROCMGR_AID_MEM_ADD, PROCMGR_AID_MEM_SPECIAL, PROCMGR_AID_MEM_GLOBAL,
        PROCMGR_AID_REBOOT, PROCMGR_AID_CLOCKSET, PROCMGR_AID_MOUNTIFS,
        PROCMGR_AID_SPAWN, PROCMGR_AID_FORK, PROCMGR_AID_XPROCESS_DEBUG,
        PROCMGR_AID_XPROCESS_MEM_READ, PROCMGR_AID_CONFSET, PROCMGR_AID_POWER,
        PROCMGR_AID_CHROOT, PROCMGR_AID_TRACE, PROCMGR_AID_RLIMIT,
        PROCMGR_AID_CPUMODE, PROCMGR_AID_RUNSTATE, PROCMGR_AID_SESSION,
        PROCMGR_AID_SETGID, PROCMGR_AID_SETUID,
    };
    uid_t uid = 0;
    gid_t gid = 0;

    if (user != NULL) {
        struct passwd *pw = getpwnam(user);
        if (pw == NULL) {
            LOGE("unknown user '%s'", user);
            exit(1);
        }
        uid = pw->pw_uid;
        gid = pw->pw_gid;
        if (setgroups(0, NULL) == -1 || setgid(gid) == -1 || setuid(uid) == -1) {
            LOGE("cannot switch to %s: %s", user, strerror(errno));
            exit(1);
        }
    }
    for (size_t i = 0; i < sizeof(deny) / sizeof(deny[0]); i++) {
        int r = procmgr_ability(0, PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT |
                                   PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | deny[i],
                                PROCMGR_AID_EOL);
        if (r != EOK)
            LOGW("procmgr_ability deny %u: %s", deny[i], strerror(r));
    }
    LOGI("privileges dropped (uid=%d gid=%d, pathspace/setuid/io/... denied+locked)",
         (int)getuid(), (int)getgid());
}

/* ----------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr,
"usage: devnats [options]\n"
"  -s url[,url]   NATS server(s)            (default nats://10.0.2.2:4222)\n"
"  -m path        mount point               (default /dev/nats)\n"
"  -q n           per-open queue bound      (default %d, oldest dropped)\n"
"  -r             raw mode by default (no line splitting / newline add)\n"
"  -U user        run as this user after attaching (e.g. qnxuser)\n"
"  -T             require TLS               -C file  CA certificate(s)\n"
"  -c file        client certificate        -k file  client key\n"
"  -H name        expected TLS hostname     -N file  NATS .creds file\n"
"  -n name        connection name           -t n     max dispatch threads (8)\n"
"  -d             daemonize                 -v       verbose (-vv debug)\n",
        DN_DEFAULT_QMAX);
}

int main(int argc, char **argv)
{
    const char *servers = "nats://10.0.2.2:4222", *mount_path = DEVNATS_DEFAULT_MOUNT;
    const char *user = NULL, *cacert = NULL, *cert = NULL, *key = NULL;
    const char *tlshost = NULL, *creds = NULL, *name = "devnats";
    bool tls = false, daemonize = false;
    int hi = 8, opt;
    natsOptions *opts = NULL;
    natsStatus st;
    resmgr_attr_t rattr;
    thread_pool_attr_t pattr;
    thread_pool_t *tpp;
    sigset_t sigs;
    pthread_t sigtid;

    while ((opt = getopt(argc, argv, "s:m:q:rU:TC:c:k:H:N:n:t:dvh")) != -1) {
        switch (opt) {
        case 's': servers = optarg; break;
        case 'm': mount_path = optarg; break;
        case 'q': g_qmax = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'r': g_raw_default = true; break;
        case 'U': user = optarg; break;
        case 'T': tls = true; break;
        case 'C': cacert = optarg; tls = true; break;
        case 'c': cert = optarg; break;
        case 'k': key = optarg; break;
        case 'H': tlshost = optarg; break;
        case 'N': creds = optarg; break;
        case 'n': name = optarg; break;
        case 't': hi = atoi(optarg); break;
        case 'd': daemonize = true; break;
        case 'v': g_verbose++; break;
        default: usage(); return opt == 'h' ? 0 : 1;
        }
    }
    if (g_qmax == 0) g_qmax = DN_DEFAULT_QMAX;
    if (hi < 2) hi = 2;

    if (daemonize && procmgr_daemon(EXIT_SUCCESS, PROCMGR_DAEMON_NOCLOSE) == -1) {
        perror("procmgr_daemon");
        return 1;
    }
    dn_slog_init();

    /* all threads (pool, nats.c) inherit a blocked SIGTERM/SIGINT/SIGHUP */
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* ---- NATS connection (retries in the background if the server is down) */
    if (nats_Open(-1) != NATS_OK) {
        LOGE("nats_Open failed");
        return 1;
    }
    st = natsOptions_Create(&opts);
    if (st == NATS_OK) {
        char *list = strdup(servers), *save = NULL, *tok;
        const char *urls[16];
        int n = 0;
        for (tok = strtok_r(list, ",", &save); tok && n < 16; tok = strtok_r(NULL, ",", &save))
            urls[n++] = tok;
        st = natsOptions_SetServers(opts, urls, n);
        if (st == NATS_OK) st = natsOptions_SetName(opts, name);
        if (st == NATS_OK) st = natsOptions_SetMaxReconnect(opts, -1);
        if (st == NATS_OK) st = natsOptions_SetReconnectWait(opts, 500);
        if (st == NATS_OK) st = natsOptions_SetRetryOnFailedConnect(opts, true, on_connected, NULL);
        if (st == NATS_OK) st = natsOptions_SetDisconnectedCB(opts, on_disconnected, NULL);
        if (st == NATS_OK) st = natsOptions_SetReconnectedCB(opts, on_reconnected, NULL);
        /* deliver all subscriptions from a small shared thread pool */
        if (st == NATS_OK) st = natsOptions_UseGlobalMessageDelivery(opts, true);
        if (st == NATS_OK && tls) st = natsOptions_SetSecure(opts, true);
        if (st == NATS_OK && cacert) st = natsOptions_LoadCATrustedCertificates(opts, cacert);
        if (st == NATS_OK && cert && key) st = natsOptions_LoadCertificatesChain(opts, cert, key);
        if (st == NATS_OK && tlshost) st = natsOptions_SetExpectedHostname(opts, tlshost);
        if (st == NATS_OK && creds) st = natsOptions_SetUserCredentialsFromFiles(opts, creds, NULL);
        free(list);
    }
    if (st == NATS_OK)
        st = natsConnection_Connect(&g_nc, opts);
    if (st == NATS_NOT_YET_CONNECTED) {
        LOGW("NATS server not reachable yet, will keep retrying");
        st = NATS_OK;
    }
    if (st != NATS_OK) {
        LOGE("NATS setup failed: %s", natsStatus_GetText(st));
        return 1;
    }
    if (natsConnection_Status(g_nc) == NATS_CONN_STATUS_CONNECTED) {
        char url[128] = "";
        natsConnection_GetConnectedUrl(g_nc, url, sizeof(url));
        LOGI("connected to %s", url);
    }

    /* ---- resource manager */
    if ((g_dpp = dispatch_create()) == NULL) {
        LOGE("dispatch_create: %s", strerror(errno));
        return 1;
    }
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_cfuncs, _RESMGR_IO_NFUNCS, &g_iofuncs);
    g_cfuncs.open = dn_open;
    g_iofuncs.read = dn_read;
    g_iofuncs.write = dn_write;
    g_iofuncs.notify = dn_notify;
    g_iofuncs.unblock = dn_unblock;
    g_iofuncs.close_ocb = dn_close_ocb;
    g_iofuncs.devctl = dn_devctl;

    memset(&g_ocbfuncs, 0, sizeof(g_ocbfuncs));
    g_ocbfuncs.nfuncs = _IOFUNC_NFUNCS;
    g_ocbfuncs.ocb_calloc = dn_ocb_calloc;
    g_ocbfuncs.ocb_free = dn_ocb_free;
    memset(&g_mount, 0, sizeof(g_mount));
    g_mount.funcs = &g_ocbfuncs;
    g_mount.conf = IOFUNC_PC_NO_TRUNC;

    iofunc_attr_init(&g_root.hdr, S_IFDIR | 0755, NULL, NULL);
    g_root.hdr.mount = &g_mount;
    g_root.hdr.inode = 1;
    g_root.is_root = true;

    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max = 4;
    rattr.msg_max_size = 16384;
    g_rid = resmgr_attach(g_dpp, &rattr, mount_path, _FTYPE_ANY, _RESMGR_FLAG_DIR,
                          &g_cfuncs, &g_iofuncs, &g_root);
    if (g_rid == -1) {
        LOGE("resmgr_attach(%s): %s", mount_path, strerror(errno));
        return 1;
    }
    LOGI("attached %s, servers %s, qmax %u, %s mode", mount_path, servers,
         (unsigned)g_qmax, g_raw_default ? "raw" : "text");

    drop_privileges(user);

    pthread_create(&sigtid, NULL, signal_thread, &sigs);

    /* ---- thread pool */
    memset(&pattr, 0, sizeof(pattr));
    pattr.handle = g_dpp;
    pattr.context_alloc = dispatch_context_alloc;
    pattr.block_func = dispatch_block;
    pattr.unblock_func = dispatch_unblock;
    pattr.handler_func = dispatch_handler;
    pattr.context_free = dispatch_context_free;
    pattr.lo_water = 2;
    pattr.hi_water = (unsigned short)(hi > 3 ? hi - 1 : 2);
    pattr.increment = 1;
    pattr.maximum = (unsigned short)hi;
    if ((tpp = thread_pool_create(&pattr, POOL_FLAG_EXIT_SELF)) == NULL) {
        LOGE("thread_pool_create: %s", strerror(errno));
        return 1;
    }
    thread_pool_start(tpp);   /* does not return (POOL_FLAG_EXIT_SELF) */
    return 0;
}
