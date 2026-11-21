/*
 * qnx-smoke: end-to-end check of the nats.c client on QNX 8.0.
 *
 *   1. core pub/sub      (publish N msgs, sync subscriber receives them)
 *   2. request/reply     (async responder + natsConnection_RequestString)
 *   3. JetStream         (file-backed stream, N acked publishes,
 *                         durable pull consumer, Fetch + Ack)
 *   4. Key/Value         (bucket create, put, get, revision)
 *
 * Usage: qnx-smoke [nats://host:4222] [count]
 * Exit code 0 only if every step passes.
 *
 * Copyright 2026 nats-qnx authors. Licensed under the Apache License 2.0.
 */
#include <nats/nats.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#define CHECK(s, what) do { if ((s) != NATS_OK) { \
    fprintf(stderr, "FAIL %s: %s\n", (what), natsStatus_GetText(s)); \
    nats_PrintLastErrorStack(stderr); failures++; goto done; } } while (0)

static int failures = 0;

static void onRequest(natsConnection *nc, natsSubscription *sub, natsMsg *m, void *c)
{
    char reply[512];
    struct utsname u;
    uname(&u);
    snprintf(reply, sizeof(reply), "pong from %.32s %.32s %.32s (pid %d): %.*s",
             u.sysname, u.release, u.machine, (int)getpid(),
             natsMsg_GetDataLength(m) > 200 ? 200 : natsMsg_GetDataLength(m), natsMsg_GetData(m));
    natsConnection_PublishString(nc, natsMsg_GetReply(m), reply);
    natsMsg_Destroy(m);
}

int main(int argc, char **argv)
{
    const char *url = argc > 1 ? argv[1] : "nats://10.0.2.2:4222";
    int n = argc > 2 ? atoi(argv[2]) : 5;
    natsConnection *nc = NULL;
    natsSubscription *sub = NULL, *rsub = NULL, *psub = NULL;
    natsMsg *msg = NULL;
    jsCtx *js = NULL;
    jsErrCode jerr = 0;
    kvStore *kv = NULL;
    kvEntry *e = NULL;
    natsStatus s;
    struct utsname u;
    char buf[128];
    int i;

    uname(&u);
    printf("qnx-smoke: nats.c %s on %s %s %s, server %s\n",
           nats_GetVersion(), u.sysname, u.release, u.machine, url);

    s = natsConnection_ConnectTo(&nc, url);
    CHECK(s, "connect");
    natsConnection_GetConnectedServerId(nc, buf, sizeof(buf));
    printf("[connect] OK server_id=%.12s...\n", buf);

    /* 1. core pub/sub */
    s = natsConnection_SubscribeSync(&sub, nc, "qnx.smoke.core");
    CHECK(s, "subscribe");
    for (i = 1; i <= n && s == NATS_OK; i++) {
        snprintf(buf, sizeof(buf), "core message %d from QNX", i);
        s = natsConnection_PublishString(nc, "qnx.smoke.core", buf);
    }
    CHECK(s, "publish");
    for (i = 1; i <= n; i++) {
        s = natsSubscription_NextMsg(&msg, sub, 5000);
        CHECK(s, "next msg");
        printf("[pubsub] recv %s: %.*s\n", natsMsg_GetSubject(msg),
               natsMsg_GetDataLength(msg), natsMsg_GetData(msg));
        natsMsg_Destroy(msg); msg = NULL;
    }
    printf("[pubsub] OK %d/%d\n", n, n);

    /* 2. request/reply */
    s = natsConnection_Subscribe(&rsub, nc, "qnx.smoke.ping", onRequest, NULL);
    CHECK(s, "responder");
    s = natsConnection_RequestString(&msg, nc, "qnx.smoke.ping", "ping", 5000);
    CHECK(s, "request");
    printf("[reqrep] OK reply: %.*s\n", natsMsg_GetDataLength(msg), natsMsg_GetData(msg));
    natsMsg_Destroy(msg); msg = NULL;

    /* 3. JetStream */
    s = natsConnection_JetStream(&js, nc, NULL);
    CHECK(s, "jetstream ctx");
    {
        jsStreamConfig sc;
        jsStreamInfo *si = NULL;
        const char *subjects[] = {"qnx.smoke.js.>"};
        js_DeleteStream(js, "QNX_SMOKE", NULL, NULL);   /* fresh run */
        jsStreamConfig_Init(&sc);
        sc.Name = "QNX_SMOKE";
        sc.Subjects = subjects;
        sc.SubjectsLen = 1;
        sc.Storage = js_FileStorage;
        s = js_AddStream(&si, js, &sc, NULL, &jerr);
        CHECK(s, "add stream");
        printf("[jetstream] stream %s created (file storage)\n", si->Config->Name);
        jsStreamInfo_Destroy(si);
    }
    for (i = 1; i <= n; i++) {
        jsPubAck *pa = NULL;
        snprintf(buf, sizeof(buf), "{\"seq\":%d,\"src\":\"qnx\",\"temp_c\":%d}", i, 20 + i);
        s = js_Publish(&pa, js, "qnx.smoke.js.telemetry", buf, (int)strlen(buf), NULL, &jerr);
        CHECK(s, "js publish");
        printf("[jetstream] published seq=%llu stream=%s\n",
               (unsigned long long)pa->Sequence, pa->Stream);
        jsPubAck_Destroy(pa);
    }
    {
        jsSubOptions so;
        natsMsgList list = {0};
        int got = 0, tries = 0;
        jsSubOptions_Init(&so);
        so.Stream = "QNX_SMOKE";
        s = js_PullSubscribe(&psub, js, "qnx.smoke.js.>", "qnx-smoke-durable", NULL, &so, &jerr);
        CHECK(s, "pull subscribe");
        while (got < n && tries++ < 10) {
            s = natsSubscription_Fetch(&list, psub, n - got, 5000, &jerr);
            if (s == NATS_TIMEOUT) continue;
            CHECK(s, "fetch");
            for (i = 0; i < list.Count; i++) {
                jsMsgMetaData *md = NULL;
                natsMsg_GetMetaData(&md, list.Msgs[i]);
                printf("[jetstream] consumed stream_seq=%llu: %.*s\n",
                       md ? (unsigned long long)md->Sequence.Stream : 0ULL,
                       natsMsg_GetDataLength(list.Msgs[i]), natsMsg_GetData(list.Msgs[i]));
                jsMsgMetaData_Destroy(md);
                natsMsg_Ack(list.Msgs[i], NULL);
                got++;
            }
            natsMsgList_Destroy(&list);
        }
        if (got != n) { fprintf(stderr, "FAIL consumed %d/%d\n", got, n); failures++; goto done; }
        printf("[jetstream] OK published+consumed+acked %d/%d\n", got, n);
    }

    /* 4. Key/Value */
    {
        kvConfig kc;
        uint64_t rev = 0;
        js_DeleteKeyValue(js, "qnx_smoke_kv");
        kvConfig_Init(&kc);
        kc.Bucket = "qnx_smoke_kv";
        kc.History = 5;
        s = js_CreateKeyValue(&kv, js, &kc);
        CHECK(s, "kv create");
        s = kvStore_PutString(&rev, kv, "board.os", u.sysname);
        CHECK(s, "kv put");
        s = kvStore_Get(&e, kv, "board.os");
        CHECK(s, "kv get");
        printf("[kv] OK %s = %s (revision %llu)\n", kvEntry_Key(e),
               kvEntry_ValueString(e), (unsigned long long)kvEntry_Revision(e));
    }

done:
    kvEntry_Destroy(e);
    kvStore_Destroy(kv);
    natsSubscription_Destroy(psub);
    natsSubscription_Destroy(rsub);
    natsSubscription_Destroy(sub);
    jsCtx_Destroy(js);
    natsConnection_Destroy(nc);
    nats_Close();
    printf("qnx-smoke: %s\n", failures ? "FAILED" : "ALL OK");
    return failures ? 1 : 0;
}
