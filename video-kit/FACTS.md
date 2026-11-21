# nats-qnx — Verified Facts & Video Kit Manifest

> **For Animator:** Everything below comes straight from `proof/proof-x86_64.txt`,
> `proof/proof-devnats.txt`, `results/summary.tsv`, and the live clips in
> `video-kit/clips/`. Do not invent numbers, hardware results, or live URLs.

## One-paragraph pitch

**nats-qnx** brings [NATS](https://nats.io) (pub/sub, request/reply, JetStream
persistence, Key/Value, and TLS) natively to **QNX Neutrino 8.0** in two layers:
a cross-compiled port of the official **nats.c v3.15.0** library with 5 upstream-grade
fixes, and **`devnats`**, an idiomatic QNX resource manager that mounts
**`/dev/nats`** so any QNX process—even a shell script or legacy C binary—can
publish, subscribe, wake on real-time **`ionotify()` pulses**, and issue
JetStream/request-reply **`devctl()`** calls over plain POSIX file descriptors.

## Hackathon context

* **Event:** QNX Global Hackathon (`qnx.devpost.com`, deadline 16 Dec 2026), built
  with QNX SDP 8.0 non-commercial licence on a Linux dev host + QNX 8.0 QEMU target.
* **Intended repository URL:** `https://github.com/yohunshadow/nats-qnx`
  (**NOT YET LIVE / NOT PUSHED** — currently local only at `/workspace/nats-qnx`).

## Architecture

1. **Layer 1 — `libnats` on QNX 8.0 (`patches/0001`–`0005`):**
   * `nats.c` **v3.15.0** (Apache-2.0) built as shared (`libnats.so.3.15.0`, 601 KB)
     and static (`libnats_static.a`) libraries with OpenSSL **3.0.22** TLS.
   * 5 minimal patches: remove `-lrt` (`0001`, clock/timers live in QNX libc),
     guard `<execinfo.h>` (`0002`), fix `printf("%s", NULL)` crashes in test-suite
     (`0003`) and inside the library's own error paths (`0004`), and retry
     `accept()` on `ECONNABORTED` (`0005`).
2. **Layer 2 — `devnats` resource manager (`/dev/nats`, 39 KB binary):**
   * Single-process QNX `resmgr` (`resmgr_attach` with `_RESMGR_FLAG_DIR` on
     `/dev/nats`, thread pool, `iofunc_*` locking) linking `libnats` with one
     shared auto-reconnecting NATS connection (`natsOptions_UseGlobalMessageDelivery`).
   * **Path → subject mapping:** `/dev/nats/a/b/c` maps to `a.b.c`; dots inside
     segments are kept (`/dev/nats/qnx/sensors.temp` → `qnx.sensors.temp`);
     `*` and `>` wildcards are read-only (writes return `EINVAL`).
   * **POSIX I/O:** `write()` publishes (text mode splits lines by `\n` by default;
     `-r` / `DCMD_NATS_SET_RAW` for binary framing); `read()` returns one message
     per call, blocking via `_RESMGR_NOREPLY` + `MsgReplyv()` from the NATS
     callback thread without tying up a pool thread (`O_NONBLOCK` → `EAGAIN`,
     signals → `EINTR` via `io_unblock`). Bounded per-fd ring (default 256) drops
     oldest on overflow and tracks `drops`.
   * **Real-time event delivery (`ionotify` + pulses):** `iofunc_notify` +
     `iofunc_notify_trigger()` delivers registered `MsgDeliverEvent()` pulses to
     `MsgReceivePulse()` clients (and gives `select()`/`poll()` for free).
     Example client `nats-notify` runs under `SCHED_FIFO`.
   * **`devctl()` API (`devnats/devnats.h`):** `DCMD_NATS_GET_STATS`, `_SET_QMAX`,
     `_SET_RAW`, `_SET_QGROUP` (queue groups), `_REQUEST` (req/reply with timeout),
     `_JS_PUBLISH` (JetStream publish + PubAck stream/seq/duplicate + Msg-Id),
     `_LAST_MSGINFO` (timestamps, subject, reply), `_FLUSH`.
   * **Hardening & ops:** `slog2` logging (`slog2info -b devnats`),
     `procmgr_ability()` privilege drop + lock (26 abilities denied/locked after
     attach, plus optional `-U user` setuid), clean `SIGTERM` shutdown (detaches
     `/dev/nats`, sends EOF to blocked readers, closes NATS).

## Verified versions & environment

* **Target OS:** QNX Neutrino **8.0.0** (QNX SDP 8.0, build `2026/10/30-09:10:05EDT`)
* **Toolchain:** `qcc` / **GCC 12.2.0** (`gcc_ntox86_64`, `gcc_ntoaarch64le`), CMake 3.31, Ninja
* **Architectures:** `x86_64` (built & tested in QEMU), `aarch64le` (cross-compiled & linked; not yet run on hardware)
* **Client library & TLS:** `nats.c` **v3.15.0**, OpenSSL **3.0.22**
* **Broker & CLI (Linux 6.12.113+ dev host):** `nats-server` **v2.15.3** (`0.0.0.0:4222 -js`), `nats` CLI **v0.5.2**
* **Virtualization:** QEMU `x86_64` (`mkqnximage`) running in **TCG** software emulation (no KVM)

## Exact verified test & benchmark numbers

### 1. Upstream `nats.c` unit test suite on QNX 8.0 (`test-harness/`)
* **329 / 336 passed**, **0 timed out** on the QNX 8.0 x86_64 QEMU guest
  (real `nats-server` processes orchestrated on the Linux host).
* The 7 failures are IPv6 user-net / QEMU-TCG timer-speed / harness log-sync
  quirks, not library build breaks (see `NOTES.md`).

### 2. `devnats` functional test suite (`devnats/tests/test-devnats.sh`)
* **29 / 29 passed** (`proof/proof-devnats.txt`, TAP output):
  attach & privilege drop, `echo`/`cat` text round-trip, dotted path mapping,
  wildcard read & write rejection, `O_NONBLOCK` `EAGAIN`, `SIGALRM` unblock (`EINTR`),
  `select()` & `poll()`, `ionotify` pulse client (`SCHED_FIFO 30`), bounded queue
  drop counter, stats, `DCMD_NATS_REQUEST` (with `no responders` error handling),
  `DCMD_NATS_JS_PUBLISH` ack + `Msg-Id` deduplication, queue groups across 2
  readers, `slog2`, clean `SIGTERM` EOF/detach, `-U qnxuser` uid 1000, raw mode.

### 3. Latency (`nats-lat`, QNX guest under QEMU TCG — no KVM)
*Caveat to display clearly on screen:* QEMU TCG software emulation inflates
absolute microseconds; compare modes against each other.

**Full proof run (`proof/proof-devnats.txt`, 500 messages, 5 ms interval, `SCHED_FIFO` 20):**

| Mode | Wake p50 | Wake p99 | End-to-End p50 | End-to-End p99 |
|---|---:|---:|---:|---:|
| Blocking `read()` | 89.7 µs | 144.0 µs | 1277.4 µs | 2110.2 µs |
| **`ionotify` + pulse** | **205.2 µs** | **339.5 µs** | **1390.0 µs** | **3547.9 µs** |
| Polling loop (1-tick sleep) | 1907.3 µs | 2371.4 µs | 3076.1 µs | 3427.5 µs |
| Pulse RTT (guest → host echo → guest) | 202.3 µs | 346.3 µs | 1651.6 µs | 2473.9 µs |

* **`ionotify` + pulse wakes ~9.3× faster at p50 than polling** (205.2 µs vs 1907.3 µs;
  live video clip `06-latency.mp4` shows 206.2 µs vs 1933.6 µs = **~9.4× faster**).

## Recorded footage clips (`video-kit/clips/`)

All clips are 1920×1080 H.264 30 fps (`yuv420p`), `#171b21` (`github-dark`) background,
recorded live from the real QNX 8.0 QEMU guest and Linux host (`casts/*.cast` included):

| Clip file | Duration | What it shows |
|---|---:|---|
| `clips/01-boot-devnats.mp4` | 9.97 s | QNX 8.0 `uname -a`, binary/lib sizes (39K / 601K), `devnats -v &` attaching `/dev/nats` & dropping privileges, `pidin` |
| `clips/02-shell-pubsub.mp4` | 11.20 s | 3-pane live split: QNX `cat /dev/nats/qnx/demo` + QNX `echo` publisher + Linux host `nats sub 'qnx.>'` receiving both `qnx.demo` and `qnx.sensors.temp` |
| `clips/03-host-to-guest.mp4` | 8.63 s | 2-pane live split: Linux host `nats pub host.cmd "start motor"` / `"set rpm 1800"` / `"stop motor"` appearing live on QNX `cat /dev/nats/host/cmd` |
| `clips/04-ionotify-pulse.mp4` | 11.00 s | 2-pane live split on QNX: `nats-notify -p 30 -n 4` (`SCHED_FIFO 30`) waking on 4 pulses (`brake=42bar`, `steer=12.4`, `throttle=85`, `gear=D`) |
| `clips/05-devctl-jetstream.mp4` | 14.17 s | `nats-ctl req` (`svc.echo`, `svc.time`), `nats-ctl jspub` (`DEVNATS` stream ack `duplicate=0` then `duplicate=1`), `nats-ctl stats` |
| `clips/06-latency.mp4` | 19.37 s | Live `nats-lat` comparison (`poll` 1933.6 µs → `pulse` 206.2 µs → `block` 87.9 µs → host RTT 1667.6 µs) |
