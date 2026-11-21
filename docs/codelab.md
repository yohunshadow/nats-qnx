# Codelab (draft): NATS on QNX 8.0 — from cross-compiling nats.c to a `/dev/nats` resource manager

> **Status:** draft. Steps were run on a Linux x86_64 host with QNX SDP 8.0 and
> the QNX 8.0 x86_64 QEMU image (TCG). Times and output come from
> `proof/proof-x86_64.txt` and `proof/proof-devnats.txt`.

**What you'll build:** a NATS client library for QNX, a NATS server on your
dev host, and `devnats`, a QNX resource manager that turns NATS subjects into
files: `echo hi > /dev/nats/sensors/temp` publishes and
`cat /dev/nats/sensors/temp` subscribes. Then you'll write a real-time client
that sleeps in `MsgReceivePulse()` until a message arrives.

**What you'll learn**
* Cross-compiling a CMake C project with `qcc` (toolchain file, patches).
* Running a QNX 8.0 target in QEMU and talking to the host network.
* The QNX resource-manager model: path-space, `iofunc_*`, OCBs, thread pools.
* Blocking reads without a thread per client (`_RESMGR_NOREPLY` + `MsgReply`).
* `ionotify()` and pulses — QNX's event delivery — and why they beat polling.

**What you need**
* Linux host, `cmake` ≥ 3.13, `ninja`, `git`, `qemu-system-x86_64`.
* QNX SDP 8.0 (free non-commercial licence from myQNX) in `~/qnx800`.
* ~1 h.

---

## 1. Build nats.c for QNX (10 min)

```bash
git clone <this repo> nats-qnx && cd nats-qnx
export QNX_SDP=$HOME/qnx800
./build.sh x86_64          # or aarch64le, or all
```

What `build.sh` does:
1. `source $QNX_SDP/qnxsdp-env.sh` — puts `qcc` on `PATH`, sets `QNX_TARGET`.
2. Clones nats.c `v3.15.0` and applies `patches/000*.patch`:
   * `0001` QNX has no `librt` — clock/timer functions live in libc.
   * `0002` no `<execinfo.h>` on QNX.
   * `0003`/`0004` `printf("%s", NULL)` crashes on QNX's libc (glibc prints
     `(null)`) — two of these were in the library's error paths.
   * `0005` `accept()` can return `ECONNABORTED` on QNX; retry.
3. Configures with `cmake/qnx-toolchain.cmake` (`CMAKE_SYSTEM_NAME=QNX`,
   `CMAKE_C_COMPILER=qcc`, `-Vgcc_ntox86_64`) and installs to `out/x86_64/`.

> **Tip:** `qcc -V` lists the target variants. `gcc_ntoaarch64le` is the
> Raspberry Pi 4/5 target.

## 2. Run the target in QEMU and start a NATS server (10 min)

```bash
mkqnximage --type=qemu --arch=x86_64 --ssh-ident=... --run   # SDP tool
scripts/host-nats-server.sh start     # nats-server -a 0.0.0.0 -p 4222 -js
./deploy.sh x86_64                    # -> /data/nats on the guest
```

QEMU user-mode networking puts the host at **10.0.2.2** inside the guest.
The server must listen on `0.0.0.0`, not `127.0.0.1`, or the guest can't
reach it.

```bash
ssh -p 2222 root@127.0.0.1
export LD_LIBRARY_PATH=/data/nats/lib
/data/nats/bin/qnx-smoke nats://10.0.2.2:4222 5
# ... [pubsub] OK 5/5, [reqrep] OK, [jetstream] OK, [kv] OK, qnx-smoke: ALL OK
```

## 3. Design a resource manager (concepts, 10 min)

On QNX, `open("/dev/nats/a/b")` is a **message** to whichever process
registered `/dev/nats` in the path-space. That process — a resource manager
— handles `open`, `read`, `write`, `devctl`, `notify` messages and replies.
Nothing runs in the kernel; a crash only kills the server.

Our mapping:

| POSIX call | devnats does |
|---|---|
| `open(path, O_WRONLY)` | turn the path into a subject (`a/b` → `a.b`) |
| `write(fd, buf)` | `natsConnection_Publish(subject, line)` |
| `open(path, O_RDONLY)` | `natsConnection_Subscribe(subject, on_msg)` |
| `read(fd)` | pop one message from the fd's queue (block if empty) |
| `ionotify/select/poll` | arm an event; fire it when a message is queued |
| `devctl(fd, DCMD_NATS_REQUEST)` | `natsConnection_Request()` |
| `close(fd)` | unsubscribe |

## 4. Write the resource manager (20 min)

Full source: `devnats/devnats.c`. The key pieces (excerpts simplified):

**4.1 Extend the OCB and attribute.** Define the types *before* including
the headers, so `iofunc_*` helpers take your structs directly:

```c
struct dn_attr; struct dn_ocb;
#define IOFUNC_ATTR_T   struct dn_attr
#define IOFUNC_OCB_T    struct dn_ocb
#define RESMGR_HANDLE_T struct dn_attr
#define RESMGR_OCB_T    struct dn_ocb
#include <sys/iofunc.h>
#include <sys/dispatch.h>

struct dn_ocb {
    iofunc_ocb_t      hdr;          /* must be first */
    pthread_mutex_t   mu;
    natsSubscription *sub;
    struct msg      **q; unsigned qhead, qlen, qmax;   /* bounded ring */
    int               waiters[16]; unsigned nwait;    /* blocked rcvids */
    iofunc_notify_t   notify[3];
    uint64_t          drops;
};
```

**4.2 Attach the path** as a directory so every `/dev/nats/...` lookup
comes to you:

```c
iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
connect_funcs.open = dn_open;
io_funcs.read  = dn_read;  io_funcs.write = dn_write;
io_funcs.notify = dn_notify; io_funcs.devctl = dn_devctl;
io_funcs.unblock = dn_unblock;
resmgr_attach(dpp, &rattr, "/dev/nats", _FTYPE_ANY, _RESMGR_FLAG_DIR,
              &connect_funcs, &io_funcs, &root_attr);
```

**4.3 Block a reader without blocking a thread.** If the queue is empty,
remember the client's `rcvid` and return `_RESMGR_NOREPLY` — the pool thread
goes back to work, the client stays REPLY-blocked:

```c
if (o->qlen == 0) {
    if (o->hdr.ioflag & O_NONBLOCK) return EAGAIN;
    o->waiters[o->nwait++] = ctp->rcvid;
    return _RESMGR_NOREPLY;
}
```

The nats.c callback (another thread) answers the waiter directly:

```c
static void on_msg(natsConnection *nc, natsSubscription *s, natsMsg *m, void *arg)
{
    struct dn_ocb *o = arg;
    pthread_mutex_lock(&o->mu);
    if (o->nwait) {                         /* a reader is waiting */
        int rcvid = o->waiters[0]; /* shift ... */
        SETIOV(&iov[0], data, len);
        MsgReplyv(rcvid, len, iov, 1);
    } else {
        enqueue(o, m);                      /* drop oldest if full */
        iofunc_notify_trigger(o->notify, o->qlen, IOFUNC_NOTIFY_INPUT);
    }
    pthread_mutex_unlock(&o->mu);
}
```

**4.4 Let signals unblock readers.** When a blocked client gets a signal,
the kernel sends an *unblock pulse*; reply `EINTR` only if the client really
asked (`_NTO_MI_UNBLOCK_REQ`), otherwise you'd race a real reply.

**4.5 Drop privileges and shut down cleanly.** After `resmgr_attach()`,
call `procmgr_ability()` to deny + lock what the daemon no longer needs
(path-space, spawn, I/O privileges, ...). A thread doing `sigwait()` for
SIGTERM detaches the path, replies EOF to all waiters and closes NATS.

Build it:

```bash
qcc -Vgcc_ntox86_64 -O2 -Wall -Iout/x86_64/include -o devnats devnats/devnats.c \
    -Lout/x86_64/lib -lnats -lssl -lcrypto -lsocket -lslog2
```

Try it on the guest:

```sh
devnats -v &
cat /dev/nats/qnx/demo &
echo hello > /dev/nats/qnx/demo          # cat prints "hello"
# on the host:  nats sub 'qnx.>'   /   nats pub qnx.demo hi
```

## 5. ionotify and pulses: an event-driven real-time client (10 min)

Polling (`read` + `sleep`) wastes CPU and adds up to one sleep period of
latency. QNX's answer is `ionotify()`: the client hands the resource manager
an event (a **pulse**), and the resource manager delivers it with
`MsgDeliverEvent()` when data arrives. Full source:
`devnats/examples/nats-notify.c`.

```c
int chid = ChannelCreate(_NTO_CHF_PRIVATE);
int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
struct sigevent ev;
SIGEV_PULSE_INIT(&ev, coid, prio, _PULSE_CODE_MINAVAIL, 0);
MsgRegisterEvent(&ev, fd);          /* QNX 8: events must be registered */

for (;;) {
    int r = ionotify(fd, _NOTIFY_ACTION_POLLARM, _NOTIFY_COND_INPUT, &ev);
    if (!(r & _NOTIFY_COND_INPUT)) {          /* nothing queued: sleep */
        struct _pulse p;
        MsgReceivePulse(chid, &p, sizeof(p), NULL);
    }
    while ((n = read(fd, buf, sizeof buf)) > 0)   /* fd is O_NONBLOCK */
        handle(buf, n);
}
```

Run it with SCHED_FIFO:

```
# nats-notify -p 30 -n 3 /dev/nats/qnx/pulse &
# echo p1 > /dev/nats/qnx/pulse; printf 'p2\np3\n' > /dev/nats/qnx/pulse
nats-notify: waiting on /dev/nats/qnx/pulse (pulse, SCHED_FIFO prio 30)
[pulse] #1 qnx.pulse: p1
[pulse] #2 qnx.pulse: p2
[pulse] #3 qnx.pulse: p3
```

On the resource-manager side, `io_notify` is just `iofunc_notify()` plus the
current queue length, and the trigger is the one line in `on_msg` above.
`select()` and `poll()` use the same mechanism — they work for free.

## 6. Measure (5 min)

```
nats-lat -m pulse|poll|block -w /dev/nats/lat/local -r /dev/nats/lat/local -n 500
```

QEMU TCG, p50 wake-up after devnats queued the message: blocking read
~90 µs, ionotify pulse ~205 µs, polling loop ~1900 µs. Emulated numbers —
re-measure on real hardware.

## 7. Where to go next

* JetStream pull consumers and Key/Value as paths
  (`/dev/nats/kv/<bucket>/<key>`).
* A `secpol` policy for devnats.
* Run it on a Raspberry Pi 5 (`./build.sh aarch64le`).

---
*Draft — TODO before publishing: screenshots/video, verify on aarch64
hardware, trim code excerpts to match the final source.*
