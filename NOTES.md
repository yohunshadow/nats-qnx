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

## Phase 2 design thoughts: `/dev/nats` resource manager

Goal: any QNX program (shell scripts, legacy C, other languages) can use NATS
via plain POSIX file I/O, without linking nats.c.

* A `resmgr` (`resmgr_attach` on `/dev/nats`, `iofunc_*` defaults) linked
  with libnats. One NATS connection per resmgr (shared), configured via
  command-line/`-c` file (url, creds, TLS).
* **Path = subject**: `open("/dev/nats/sensors/temp", O_WRONLY)` →
  `write()` publishes one message per write on `sensors.temp`.
  `open(..., O_RDONLY)` creates a subscription; `read()` returns one message
  per call (blocking; `O_NONBLOCK` → `EAGAIN`), `select/poll` via
  `iofunc_notify` so it integrates with QNX event loops / `ionotify()`.
  Wildcards via `*` / `>` path components (`/dev/nats/sensors/*`).
* **JetStream**: `/dev/nats/js/<stream>/<subject>` — `write()` = `js_Publish`
  with the PubAck sequence returned via `devctl()`; reads come from a durable
  pull consumer named after the opener (ack on read, or explicit `devctl`
  ack for at-least-once).
* **KV**: `/dev/nats/kv/<bucket>/<key>` — `read()` = get, `write()` = put;
  `ls /dev/nats/kv/<bucket>` lists keys (`io_read` on a directory).
* `devctl()` for headers, request/reply (`DCMD_NATS_REQUEST` with timeout),
  stats, flush.
* Message framing: default one message per `read()`/`write()`; optional
  newline-delimited mode via open flag / mount option for shell use
  (`echo hi > /dev/nats/foo`, `cat /dev/nats/foo`).
* QNX-specific: use pulses for subscription callbacks → resmgr thread pool,
  honor client priority inheritance, `secpol` type for the resmgr, and
  `procmgr_ability` to drop privileges after attaching the path.
