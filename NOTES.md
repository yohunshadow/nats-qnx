# nats-qnx — porting notes

Upstream: [nats-io/nats.c](https://github.com/nats-io/nats.c) **v3.15.0**
(commit `23f51ca6`). Target: **QNX SDP 8.0**, `qcc` (GCC 12.2.0),
`x86_64` (QEMU) and `aarch64le` (e.g. Raspberry Pi 5).

nats.c is already very POSIX-clean, so the port is small. Every change is a
separate patch in `patches/`, applied on top of the pristine upstream tag by
`build.sh`. Nothing in the library sources (`src/`) needed to change.

## Porting issues and fixes

### 1. `-lrt` does not exist on QNX (library link failure) — `0001`

*Symptom:* `x86_64-pc-nto-qnx8.0.0-ld: cannot find -lrt` when linking
`libnats.so`.

*Cause:* nats.c's CMake treats every non-Apple UNIX as Linux and links
`librt` for `clock_gettime()`. On QNX Neutrino those realtime functions are in
`libc`; there is no `librt`. Conversely, the BSD socket API (`socket`,
`connect`, `getaddrinfo`, `setsockopt` …) lives in **`libsocket`**, which is
not linked implicitly.

*Fix:* when `CMAKE_SYSTEM_NAME STREQUAL "QNX"`, set `NATS_EXTRA_LIB` to
`socket` instead of `rt`. (`NATS_OS` stays `LINUX` — nothing in `src/` keys on
it beyond generic POSIX code paths, and those all work on QNX.)

### 2. No `<execinfo.h>` on QNX (test-suite compile failure) — `0002`

*Symptom:* `test/test.h:16: fatal error: execinfo.h: No such file or
directory`.

*Cause:* the test-suite's SIGSEGV handler prints a glibc `backtrace()`. QNX
has its own `libbacktrace` (`bt_get_backtrace()` …) instead of glibc's API.

*Fix:* guard the include and the two `backtrace*()` calls with
`!defined(__QNX__)`, exactly the way upstream already guards Android.
(A future improvement could use QNX `bt_*` to keep the backtrace.)

### 3. `printf("%s", NULL)` crashes on QNX (test-suite SIGSEGV) — `0003`

*Symptom:* in the first full run on QNX, 88 tests (almost all JetStream,
KeyValue, ObjectStore and several TLS tests) died immediately with
`Error: signal 11`.

*Cause:* passing `NULL` for a `%s` conversion is undefined behaviour.
glibc prints `(null)`, so it never shows up on Linux CI; QNX's libc
dereferences the pointer. Two call sites did this:
* the "Skipping since requires server version of at least X, got %s" messages
  (and the version banner) print `serverVersion`, which is `NULL` unless
  `NATS_TEST_SERVER_VERSION` is set;
* `test_SubBadSubjectAndQueueName` prints its first bad subject, which is
  literally `NULL`.

*Fix:* a small `_nn()` helper (`NULL` → `"(null)"`) used at those sites. Also
the runner now sets `NATS_TEST_SERVER_VERSION` (as upstream CI does), otherwise
version-gated tests would *skip* instead of actually exercising JetStream.

Side observation: after that SIGSEGV the test process sometimes hung instead of
exiting (the handler calls `exit()` from a signal context while other threads
hold library locks — async-signal-unsafe on any OS, just more visible on QNX).
The runner's per-test timeout covers this.

### 4. `printf("%s", NULL)` inside the **library** (real crash bug) — `0004`

*Symptom:* after 0003, `SSLLoadCAFromMemory`, `SSLServerNameIndication`,
`SSLTLSSettingsImplySecure` and `JetStreamConvertDirectMsg` still died with
SIGSEGV — this time inside `libnats` itself.

*Cause:* same undefined behaviour, but in library error paths, so on QNX a
*recoverable* error becomes an application crash:
* `NATS_SSL_ERR_REASON_STRING` was `ERR_reason_error_string(ERR_get_error())`.
  That returns `NULL` when the OpenSSL error queue is empty and — with
  OpenSSL 3 — for system/errno errors. Triggered by e.g. an invalid CA PEM
  passed to `natsOptions_SetCATrustedCertificates()`, a missing file for
  `natsOptions_LoadCATrustedCertificates()`, or a TLS handshake aborted by the
  peer.
* `js_directGetMsgToJSMsg()` printed possibly-`NULL` header values in its
  "missing stream name / sequence / timestamp / subject" errors (malformed
  direct-get reply ⇒ crash).

*Fix:* `nats_sslErrReasonString()` (static inline in `natsp.h`, never returns
`NULL`) behind the existing `NATS_SSL_ERR_REASON_STRING` macro, plus a
`NATS_SAFE_STR()` helper for the direct-get errors. This one is worth sending
upstream: it is a latent bug on every non-glibc platform.

### 5. `accept()` returns `ECONNABORTED` for a reset connection — `0005`

*Symptom:* `SSLNoServersFromPlaintextInfo` failed its first step, even
without the test harness.

*Cause:* the test checks whether the client dialed a mock server by calling
`accept()` *after* the client has closed. nats.c sockets use
`SO_LINGER {1,0}`, so the close sends a RST. Verified with a 30-line probe on
both systems: Linux still returns the reset connection from `accept()`; QNX
8.0's io-sock (FreeBSD-derived TCP/IP stack) drops it and `accept()` fails with
`ECONNABORTED` ("Software caused connection abort").

*Fix (test-only):* treat `ECONNABORTED` as "the client did dial". The library
never calls `accept()`, so it is unaffected. Worth knowing for anyone writing
QNX servers: handle `ECONNABORTED` from `accept()` as a transient error.

### 6. Compiler warnings (no change needed)

The library itself compiles warning-free with QNX GCC 12.2. Only the
upstream *examples* show two harmless GCC-12 warnings
(`-Wuse-after-free` in `examples/micro_args.h`, and `PRIi64` used with
`int64_t / 1000000000LL`, which is `long long`, in one docs example). Neither
is QNX-specific (same on Linux GCC 12). Left untouched.

### Things that just worked

* `pthread` (mutex/cond/thread), `clock_gettime(CLOCK_MONOTONIC)`,
  `poll`/`select`-based socket I/O, non-blocking connect, `SO_LINGER`,
  `getaddrinfo` with IPv4/IPv6 ordering.
* **TLS**: OpenSSL **3.0.22** from the SDP target tree
  (`$QNX_TARGET/<arch>/usr/lib/libssl.so`, headers in
  `$QNX_TARGET/usr/include/openssl`) is found by CMake's `FindOpenSSL` with the
  toolchain file. The QNX 8.0 QEMU image already ships `libssl.so.3` /
  `libcrypto.so.3`. TLS with CA + hostname verification was verified on the
  guest. Fallback for images without OpenSSL: `NATS_TLS=OFF ./build.sh`.
* `fork()` from a multi-threaded process (used by the test-suite to spawn
  servers) works on QNX 8.0.
* Optional deps (libsodium for faster nkeys, protobuf-c for streaming) are
  not needed; streaming (STAN, deprecated) is disabled.

## CMake toolchain file (`cmake/qnx-toolchain.cmake`)

CMake ≥ 3.x has a built-in `QNX` platform and knows the `QCC` compiler id; the
toolchain file only selects `qcc`/`q++`, passes the `-V gcc_nto<arch>` target
through `CMAKE_<LANG>_COMPILER_TARGET`, and points `find_*` at
`$QNX_TARGET` + `$QNX_TARGET/<arch>` so OpenSSL is picked from the SDP rather
than from the build host. Requires `source qnxsdp-env.sh` first.

## Running the upstream unit tests on QNX (test-harness/)

Problem: the upstream suite `fork()`+`exec()`s a local `nats-server` binary
and connects to it on `127.0.0.1`. nats-server is written in Go and **Go has
no `GOOS=qnx`**, so there is no nats-server for QNX.

Solution (test-only, no change to test logic):

* **`nats-server-remote`** (QNX binary, set as `NATS_TEST_SERVER_EXE`): a
  stand-in that ships its argv (and any `-c` config file) to
  **`remote-server-ctl.py`** on the dev host, which starts a *real*
  nats-server there with the same arguments. `SIGINT` from the test's
  `_stopServer()` → shim sends `STOP` → host stops the server, waits for it to
  exit (so the port is free) and ships `-l server.log` back to the guest (some
  tests grep the server log).
* **`loopback-redirect.so`** (`LD_PRELOAD`): rewrites `connect()` to
  `127.0.0.1:<port>` into `10.0.2.2:<port>` (QEMU user-net alias of the host's
  loopback). Closed ports still give `ECONNREFUSED`, so "no server" test cases
  behave normally. Ports that the process has `bind()`-ed itself (the suite's
  in-process *mock servers*) are not redirected.
* `run-unit-tests.sh`: one test per process (like upstream `ctest`), per-test
  timeout, writes `results/summary.tsv` + per-test logs.

* QEMU TCG guest clock drift: the guest RTC fell ~74 s behind the host
  during the first ~1.5 h run, which broke JetStream tests that compare a
  client timestamp with the server's (`DeliverByStartTime`, KV delete-marker
  age …). The runner now sets the guest clock from the host (aligned to a
  second boundary, QNX `date` has 1 s resolution) before each test.

Known harness / emulator limitations from the last full run (329 / 336
passed, 0 timeouts). These are not library build failures:

* `natsSock_IPOrder`, `IPResolutionOrder` — IPv6 loopback / `::` listeners
  are not redirected (`loopback-redirect.so` only rewrites IPv4
  `127.0.0.1`); QEMU user-net IPv6 to the host is not set up.
* `GetLocalAndRemoteIPAndPort` — the client runs in the guest and the
  server on the host, so the addresses they observe for each other are
  not 127.0.0.1 on both sides.
* `natsTimer` — the "Create performance" assertion (many timers in < 1 s)
  misses under QEMU TCG (this image is not using KVM).
* Flakes seen in the full suite but passing when re-run alone:
  `AutoUnsubNoUnsubOnDestroy` (racy shipping of `server.log` from the
  host), `HotSpotReconnect` (many reconnects through SLIRP),
  `JetStreamOrderedConsumer` (950/1024 chunks under TCG load; also
  sensitive to guest clock drift — the runner now resyncs the guest RTC
  from the host before every test).

See README for the table and `results/summary.tsv` for the raw counts.

## Phase 2: `/dev/nats` resource manager (`devnats/`)

Goal: any QNX program (shell scripts, legacy C, other languages) can use NATS
through plain POSIX file I/O, without linking nats.c. Implemented in
`devnats/devnats.c` (~1000 lines, C, qcc, links libnats); public devctl API in
`devnats/devnats.h`. Proof: `proof/proof-devnats.txt` (29/29 guest tests).

### What was built (vs. the original design)

| Design item | Status |
|---|---|
| resmgr on `/dev/nats` linked with libnats, one shared connection, `-s url[,url]`, TLS (`-T -C -c -k -H`), creds (`-N`) | done |
| path = subject, write publishes, read = one message, blocking, `O_NONBLOCK` → `EAGAIN` | done |
| `select`/`poll`/`ionotify` via `iofunc_notify` | done (pulse example in `examples/nats-notify.c`) |
| `*` / `>` wildcard paths | done (read-only; write → `EINVAL`) |
| devctl: request/reply, stats, flush | done |
| devctl: JetStream publish with PubAck | done (`DCMD_NATS_JS_PUBLISH`, Msg-Id dedup) |
| queue groups | done (`DCMD_NATS_SET_QGROUP`) |
| newline framing for the shell | done — text mode is the **default**; `-r` / `DCMD_NATS_SET_RAW` for raw |
| `procmgr_ability` privilege drop after attach, optional `-U user` | done |
| `/dev/nats/js/<stream>/...` durable pull consumers, `/dev/nats/kv/...` | **not done** (future) |
| headers via devctl, secpol type | **not done** (future) |

### Path → subject mapping

* `/dev/nats/a/b/c` → subject `a.b.c` (components joined with `.`).
* Dots inside a component are kept: `/dev/nats/a/b.c` is also `a.b.c`, handy
  when a subject has many tokens.
* `*` and `>` components are NATS wildcards and only valid for `O_RDONLY`;
  `>` must be the last component. Opening a wildcard for writing, an empty
  component, or `>` in the middle fails with `EINVAL`.
* Opening `/dev/nats` itself (the directory) is allowed — it is the handle
  `nats-ctl stats` uses for global counters.

### Internals

* **Attributes / OCBs.** `IOFUNC_ATTR_T`/`IOFUNC_OCB_T` (and `RESMGR_*`,
  `THREAD_POOL_PARAM_T`) are overridden *before* the system headers so the
  iofunc helpers take `&o->hdr` without casts. The mount is a
  `_RESMGR_FLAG_DIR` attach; each `open()` gets its own attr (subject) and an
  OCB allocated through `iofunc_mount_t.funcs` (`ocb_calloc`/`ocb_free`).
* **Delivery path.** `open(O_RDONLY)` starts an async nats.c subscription
  (`natsOptions_UseGlobalMessageDelivery`, so subscriptions don't each get a
  thread). The callback copies the message into the OCB's bounded ring
  (default 256, `-q`, per fd via `DCMD_NATS_SET_QMAX`); when full the
  **oldest** message is dropped and counted (stats `drops`). If a reader is
  blocked, the callback replies to it directly with `MsgReplyv()` (up to 16
  queued rcvids per OCB), otherwise `iofunc_notify_trigger()` fires the
  armed `ionotify`/`select` events.
* **Blocking reads.** `io_read` with an empty queue returns `_RESMGR_NOREPLY`
  and stores the rcvid. `io_unblock` replies `EINTR`, but only when
  `MsgInfo()` shows `_NTO_MI_UNBLOCK_REQ` — otherwise the message is already
  on its way. `O_NONBLOCK` → `EAGAIN`.
* **Locking.** Each OCB has its own mutex for the queue/waiters; `io_notify`
  takes it too, because the nats callback thread calls
  `iofunc_notify_trigger()` outside the resmgr thread pool.
  `sub_stop()` (unsubscribe + wait for the subscription's on-complete
  callback with a cond-var) must be called **without** the OCB mutex, or the
  final callback deadlocks.
* **devctl.** `__DIOF/__DIOT/__DIOTF/__DION`, class `_DCMD_MISC`, command
  numbers 0x41-0x48 (the command field is 8 bits — larger numbers spill into
  the class field). Structures are ≤ 4 KiB + header; the thread pool's
  `msg_max_size` is 16 KiB so they arrive in one message. Failures are
  returned as the devctl error (`ETIMEDOUT`, `ENOENT` = no responders / no
  stream, `EIO`); a first version put the error in `ret_val` (= `dev_info`),
  so `devctl()` returned `EOK` with an empty reply — caught by the test suite.
* **Reconnect.** `SetRetryOnFailedConnect` + `MaxReconnect(-1)`: devnats
  attaches even if the server is down and keeps subscriptions across
  reconnects; disconnect/reconnect are logged and counted.
* **Logging.** slog2 buffer `devnats` (`slog2info -b devnats`), mirrored to
  stderr with `-v`.
* **Privileges.** After `resmgr_attach` (and the optional `-U user`
  uid/gid switch), `procmgr_ability()` denies and locks 26 abilities:
  path-space, setuid/setgid, I/O + interrupts, physical/global/special
  memory, reboot, clock set, mount, spawn/fork, cross-process debug/memory
  read, chroot, trace, rlimit, confset, power, session, run-state. The test
  suite shows the daemon running as uid 1000 and still serving.
* **Shutdown.** A dedicated thread `sigwait()`s for SIGTERM/SIGINT/SIGHUP:
  detaches the path, replies EOF (0 bytes) to every blocked reader (so `cat`
  exits 0), closes the NATS connection (flush) and exits.

### QNX gotchas hit in phase 2

* `PROCMGR_AID_KEYDATA` / `PROCMGR_AID_CLOCKPERIOD` aren't in the 8.0 headers.
* `ionotify()` clients must `MsgRegisterEvent(&ev, fd)` first in QNX 8 —
  unregistered events are rejected.
* `_NOTIFY_ACTION_POLLARM` / `_NOTIFY_COND_INPUT` need `<sys/iomsg.h>`.
* A background reader started in one `ssh` session dies with SIGHUP when that
  session ends — use `nohup` or keep reader and writer in one session (looked
  like "messages lost" at first).
* The QEMU guest's clock drifts a lot under TCG; the test runner resyncs it
  over ssh before running.

### Latency (QEMU TCG — see README for the numbers)

`examples/nats-lat` timestamps with `CLOCK_MONOTONIC` on the guest: `wake` =
reader got the data − devnats enqueued it (pure resmgr→client wake-up),
`e2e` = reader got it − writer wrote it (includes the trip through the host
nats-server). Blocking `read()` is fastest (devnats replies straight from the
callback thread); ionotify+pulse adds one pulse + re-arm + non-blocking read
(~0.1 ms more under TCG); a polling loop is bound by the sleep granularity
(1 tick) and is ~10× worse at p50. Absolute numbers are emulation-bound;
re-measure on hardware/KVM.

### Future work

* JetStream pull consumers and KV as paths (`/dev/nats/js/...`,
  `/dev/nats/kv/<bucket>/<key>`), headers via devctl.
* A secpol type for devnats and a config file (`-c`).
* Run on aarch64 hardware (binaries build), video demo, upstream PR for the
  phase 1 patches.
