# nats-qnx — NATS messaging for QNX 8.0

**nats-qnx** brings [NATS](https://nats.io), the lightweight cloud-native
messaging system, to **QNX Neutrino 8.0**. It ports the official C client
[nats.c](https://github.com/nats-io/nats.c) (v3.15.0, Apache-2.0) to QNX
SDP 8.0 so QNX targets can publish/subscribe, do request/reply, and use
**JetStream** (persistent streams, consumers, Key/Value, Object Store) with
**TLS**, talking to any NATS server or cluster.

*Why:* QNX runs the safety-critical half of cars, robots and medical devices;
NATS is increasingly how the cloud/edge half talks. A native, tested NATS
client lets a QNX ECU stream telemetry, receive commands, and persist data
through JetStream without a gateway or a custom protocol.

| | |
|---|---|
| QNX version | **QNX SDP 8.0** (QNX Neutrino 8.0.0) |
| Tested target | **x86_64**, QNX 8.0 QEMU image built with `mkqnximage` |
| Also builds for | **aarch64le** (e.g. Raspberry Pi 5) — compiled and linked, not yet run on hardware |
| Toolchain | **`qcc` / GCC 12.2.0** (`gcc_ntox86_64`, `gcc_ntoaarch64le`), CMake ≥ 3.13 (tested 3.31), Ninja |
| Upstream | nats.c **v3.15.0** + 5 small patches (`patches/`) |
| TLS | OpenSSL 3.0.22 from the SDP (`NATS_TLS=OFF` to build without) |
| Server | `nats-server` v2.15.3 on the Linux dev host (Go has no QNX port) |

On top of the library, **`devnats`** is a QNX resource manager that mounts
**`/dev/nats`**, so any QNX program — even a shell script — can use NATS
with plain POSIX file I/O: `echo hello > /dev/nats/qnx/demo` publishes,
`cat /dev/nats/qnx/demo` subscribes. See [/dev/nats](#devnats-the-devnats-resource-manager).

* **Codelab:** [Native NATS & `/dev/nats` Resource Manager on QNX 8.0](docs/codelab.md) (50–60 min step-by-step guide with code walkthrough, screenshots, and troubleshooting)
* **Demo video:** *YouTube link to be added* (recording kit in [`video-kit/`](video-kit/))

## Status

* Library: builds warning-free for x86_64 and aarch64le (shared + static),
  with TLS against the SDP's OpenSSL 3.
* On the QNX 8.0 x86_64 QEMU guest, talking to a host nats-server at
  `10.0.2.2:4222`: core pub/sub, request/reply, JetStream publish +
  push/pull consume, Key/Value, and TLS with CA + hostname verification
  all work. Full capture: [`proof/proof-x86_64.txt`](proof/proof-x86_64.txt).
* Upstream unit tests on the QNX guest: **329 / 336 passed** (0 timeouts).
  The 7 failures are harness / QEMU-TCG limitations, not library build
  breaks — see [Test results](#test-results) and [NOTES.md](NOTES.md).
* `/dev/nats` resource manager: **29 / 29** automated guest tests pass
  (write/read, wildcards, `O_NONBLOCK`, signals, `select`/`poll`/`ionotify`
  pulses, queue bound + drops, request/reply, JetStream ack, queue groups,
  slog2, privilege drop, clean SIGTERM). Interop with host `nats sub`/`nats
  pub` shown in [`proof/proof-devnats.txt`](proof/proof-devnats.txt).
* Two of the five patches fix latent upstream bugs (NULL `"%s"` in library
  error paths, and `printf("%s", NULL)` in the test-suite) that crash on
  QNX's libc even though glibc hides them.

## Repository layout

```
build.sh                    fetch nats.c tag, apply patches, cross-compile
deploy.sh                   copy lib/examples/tests to the QNX guest over SSH
cmake/qnx-toolchain.cmake   CMake toolchain file for qcc (x86_64 / aarch64le)
patches/                    QNX patches on top of upstream (git format-patch)
demo/qnx-smoke.c            end-to-end demo: pub/sub, req/reply, JetStream, KV
demo/run-proof.sh           captures proof/proof-<arch>.txt from the guest
scripts/host-nats-server.sh download + run nats-server (JetStream) on the host
test-harness/               run the upstream test-suite on QNX (see NOTES.md)
devnats/devnats.c           /dev/nats resource manager (daemon)
devnats/devnats.h           devctl API (stats, request, JetStream publish, ...)
devnats/examples/           nats-notify (ionotify+pulse), nats-ctl, nats-lat
devnats/tests/              test-devnats.sh (guest, TAP) + run-tests.sh (host)
tools/host-echo.c           host helper: lat.ping->lat.pong, svc.echo, svc.time
docs/codelab.md             step-by-step codelab: nats.c on QNX -> /dev/nats resmgr
docs/img/                   terminal screenshots referenced by the codelab
results/                    last unit-test run: summary.tsv + per-test logs
proof/                      captured guest proof output
```

`build/`, `out/` and `hostbin/` are generated and gitignored.

## Build

Prerequisites: **QNX SDP 8.0** (free non-commercial licence from myQNX;
`qcc` / **GCC 12.2.0** toolchain), plus `cmake` (≥ 3.13), `ninja` and `git`
on a Linux host. Targets: **`x86_64`** (tested on QNX 8.0 QEMU) and
**`aarch64le`** (cross-compiles and links; not yet tested on hardware).
See the full [Codelab](docs/codelab.md) for a guided walkthrough.

```bash
export QNX_SDP=$HOME/qnx800          # where SDP 8.0 is installed
./build.sh x86_64                    # or: aarch64le | all
# NATS_TLS=OFF ./build.sh x86_64     # build without OpenSSL
```

`build.sh` sources `$QNX_SDP/qnxsdp-env.sh`, clones `nats-io/nats.c` at
`v3.15.0` into `build/`, applies `patches/*.patch`, configures with
`-DCMAKE_TOOLCHAIN_FILE=cmake/qnx-toolchain.cmake -DQNX_ARCH=<arch>` and
installs to `out/<arch>/` (`lib/libnats.so.3.15.0`, `libnats_static.a`,
`include/nats/`, `bin/` examples + `qnx-smoke` + `testsuite`), then builds
`devnats` and its example clients into `out/<arch>/bin/`.

Using it from your own QNX project:

```bash
qcc -Vgcc_ntox86_64 -Iout/x86_64/include app.c \
    -Lout/x86_64/lib -lnats -lssl -lcrypto -lsocket
```

## Run on the QNX 8.0 QEMU target

1. Boot the QNX 8.0 x86_64 QEMU image (built with `mkqnximage --type=qemu
   --arch=x86_64`, user-mode networking, SSH forwarded to host port 2222).
   Inside the guest the host is reachable as `10.0.2.2`.
2. Start a NATS server with JetStream on the host, listening on all
   interfaces (the guest cannot reach a server bound only to 127.0.0.1):
   ```bash
   scripts/host-nats-server.sh start      # 0.0.0.0:4222, -js
   ```
3. Deploy and run:
   ```bash
   ./deploy.sh x86_64                     # -> /data/nats on the guest
   ssh -p 2222 root@127.0.0.1
   export LD_LIBRARY_PATH=/data/nats/lib
   /data/nats/bin/qnx-smoke nats://10.0.2.2:4222 5
   /data/nats/bin/nats-subscriber -s nats://10.0.2.2:4222 -subj demo -count 5 &
   /data/nats/bin/nats-publisher  -s nats://10.0.2.2:4222 -subj demo -count 5
   ```
4. Or capture everything at once: `demo/run-proof.sh x86_64`.

### Proof (x86_64 QEMU guest, 21 Nov 2026, 03:59 WIB)

`qnx-smoke` against `nats://10.0.2.2:4222`:

```
qnx-smoke: nats.c 3.15.0 on QNX 8.0.0 x86pc, server nats://10.0.2.2:4222
[connect] OK server_id=NCJAKNANJZ3H...
[pubsub] recv qnx.smoke.core: core message 1 from QNX
[pubsub] recv qnx.smoke.core: core message 2 from QNX
[pubsub] recv qnx.smoke.core: core message 3 from QNX
[pubsub] recv qnx.smoke.core: core message 4 from QNX
[pubsub] recv qnx.smoke.core: core message 5 from QNX
[pubsub] OK 5/5
[reqrep] OK reply: pong from QNX 8.0.0 x86pc (pid 54513685): ping
[jetstream] stream QNX_SMOKE created (file storage)
[jetstream] published seq=1 stream=QNX_SMOKE
[jetstream] published seq=2 stream=QNX_SMOKE
[jetstream] published seq=3 stream=QNX_SMOKE
[jetstream] published seq=4 stream=QNX_SMOKE
[jetstream] published seq=5 stream=QNX_SMOKE
[jetstream] consumed stream_seq=1: {"seq":1,"src":"qnx","temp_c":21}
[jetstream] consumed stream_seq=2: {"seq":2,"src":"qnx","temp_c":22}
[jetstream] consumed stream_seq=3: {"seq":3,"src":"qnx","temp_c":23}
[jetstream] consumed stream_seq=4: {"seq":4,"src":"qnx","temp_c":24}
[jetstream] consumed stream_seq=5: {"seq":5,"src":"qnx","temp_c":25}
[jetstream] OK published+consumed+acked 5/5
[kv] OK board.os = QNX (revision 1)
qnx-smoke: ALL OK
```

Core pub/sub (upstream `nats-publisher` / `nats-subscriber` on the guest):

```
Sending 5 messages to subject 'qnx.demo'
Sent 5 messages in 11 milliseconds (454 msgs/sec)
Received msg: qnx.demo - hello-from-QNX-8.0   (×5)
Received 5 messages in 2 milliseconds (2500 msgs/sec)
```

JetStream (upstream `nats-js-pub` / `nats-js-sub` on the guest; host view
`stream QNXDEMO subjects ['qnx.js'] messages 10 last_seq 10`):

```
Stream QNXDEMO has 0 messages (0 bytes)
Sending 10 messages to subject 'QNXDEMO'
Sent 10 messages in 12 milliseconds (833 msgs/sec)
Stream QNXDEMO has 10 messages (390 bytes)
Received msg: qnx.js - 'js-hello-from-QNX'   (×10)
Received 10 messages in 10 milliseconds (1000 msgs/sec)
```

The same capture also shows QNX↔Linux host pub/sub, a durable pull
consumer with ack floor 5, KV `board.os = QNX` readable from the host,
and TLS (correct CA works; a wrong CA fails with
`unable to get local issuer certificate`).

## devnats: the `/dev/nats` resource manager

```
 QNX guest                                                       Linux host
 ┌──────────────────────────────────────────────────────┐
 │ shell: echo/cat   C app: open/read/write/devctl       │
 │ RT client: ionotify + MsgReceivePulse (SCHED_FIFO)    │
 │        │   POSIX I/O = QNX messages   ▲ reply / pulse │
 │        ▼                              │               │
 │  ┌──────────── devnats (resmgr) ───────────────────┐  │
 │  │ thread pool ── io_open/read/write/notify/devctl │  │
 │  │ per-fd OCB: subject, bounded queue, waiters,    │  │   ┌──────────────┐
 │  │             drops, notify list                  │  │   │ nats-server  │
 │  │ libnats (one connection, auto-reconnect, TLS) ◄─┼──┼──►│ 0.0.0.0:4222 │
 │  │ slog2 "devnats", procmgr_ability drop, SIGTERM  │  │   │ + JetStream  │
 │  └─────────────────────────────────────────────────┘  │   └──────────────┘
 └──────────────────────────────────────────────────────┘     ▲  nats sub/pub
                                                              host-echo
```

Path → subject: `/dev/nats/a/b/c` = `a.b.c`; dots inside a component are
kept (`/dev/nats/a/b.c` = `a.b.c`); `*` / `>` components subscribe with
wildcards (read-only, `>` last).

* `write()` publishes. Text mode (default) publishes one message per line,
  newline stripped; raw mode (`-r`, or `DCMD_NATS_SET_RAW` per fd) = one
  message per `write()`.
* `read()` on an fd opened `O_RDONLY` returns **one message per call**
  (text mode appends `\n`). It blocks until a message arrives; with
  `O_NONBLOCK` it returns `EAGAIN`. Signals interrupt it (`EINTR`).
* Each fd has a bounded queue (default 256, `-q n`, `DCMD_NATS_SET_QMAX`);
  when full the oldest message is dropped and counted.
* `select()`, `poll()` and `ionotify()` work (`iofunc_notify`), so a real-time
  client can sleep in `MsgReceivePulse()` and be woken by a pulse.
* `devctl()` (see `devnats/devnats.h`): `DCMD_NATS_GET_STATS`,
  `_SET_QMAX`, `_SET_RAW`, `_SET_QGROUP` (queue group),
  `_REQUEST` (request/reply with timeout), `_JS_PUBLISH` (JetStream publish,
  returns stream + sequence + duplicate flag, optional Msg-Id),
  `_LAST_MSGINFO` (subject/reply/timestamps of the last read), `_FLUSH`.

```
devnats [-s nats://host:4222[,url2]] [-m /dev/nats] [-q 256] [-r]
        [-T] [-C ca.pem] [-c cert.pem] [-k key.pem] [-H tls-hostname]
        [-N user.creds] [-n conn-name] [-t max-threads] [-U user] [-d] [-v]
```
(default server `nats://10.0.2.2:4222`, i.e. the QEMU host; `-d` runs in the
background, `-v` mirrors the slog2 log to stderr.)

### Shell demo (from `proof/proof-devnats.txt`)

```
# guest
devnats -v &                      # attached /dev/nats, servers nats://10.0.2.2:4222
cat /dev/nats/qnx/demo &          # subscribe
echo hello > /dev/nats/qnx/demo   # publish   -> cat prints: hello

# guest -> host: host runs  nats sub 'qnx.>'
echo 'from qnx shell' > /dev/nats/qnx/demo
echo 'via dots'       > /dev/nats/qnx/sensors.temp
[#1] Received on "qnx.demo"
from qnx shell
[#2] Received on "qnx.sensors.temp"
via dots

# host -> guest: guest runs  head -n 2 /dev/nats/host/cmd
nats pub host.cmd "start motor"; nats pub host.cmd "stop motor"
guest read: start motor
guest read: stop motor

# request/reply and JetStream through devctl
nats-ctl req /dev/nats/svc/echo "hi from qnx" 3000  -> reply: echo: hi from qnx
nats-ctl jspub /dev/nats/devnats/js/test "persist me" <msg-id>
                                    -> jspub ack: stream=DEVNATS seq=4 duplicate=0
```

ionotify + pulse client (`devnats/examples/nats-notify.c`):

```
nats-notify: waiting on /dev/nats/qnx/pulse (pulse, SCHED_FIFO prio 30)
[pulse] #1 qnx.pulse: p1
[pulse] #2 qnx.pulse: p2
[pulse] #3 qnx.pulse: p3
nats-notify: received 3 message(s)
```

### Tests

```bash
./devnats/tests/run-tests.sh | tee proof/proof-devnats.txt
```
starts/checks the host server, the `DEVNATS` stream and `host-echo`, copies
the binaries, runs `devnats/tests/test-devnats.sh` on the guest (TAP, 29
checks), then the shell/interop demo and the latency runs. Last run
(21 Nov 2026, 12:27 WIB): **29/29 passed**.

### Latency

`devnats/examples/nats-lat`, 500 messages at 5 ms intervals, reader
SCHED_FIFO 20, all times in µs. `wake` = devnats queued the message → client
has it (resmgr → client wake-up only); `e2e` = client `write()` → devnats →
host nats-server → devnats → client `read()`.

| Reader | wake p50 | wake p99 | e2e p50 | e2e p99 |
|---|---:|---:|---:|---:|
| blocking `read()` | 90 | 144 | 1277 | 2110 |
| **ionotify + pulse** | 205 | 340 | 1390 | 3548 |
| polling loop (non-blocking read + 1-tick sleep) | 1907 | 2371 | 3076 | 3428 |
| pulse, guest → host echo → guest (`lat.ping`→`lat.pong`) | 202 | 346 | 1652 (RTT) | 2474 (RTT) |

ionotify pulses wake the client ~9× sooner than polling at p50 and with far
less jitter, without burning CPU. **Caveat:** the guest is QEMU **TCG**
(software emulation, no KVM), so absolute numbers are inflated — compare
modes against each other, and re-measure on hardware.

## Test results

The upstream nats.c test-suite (`test/list_test.txt`, **336** tests) runs
**on the QNX guest**, one test per process like upstream `ctest`. Because
there is no nats-server for QNX, `test-harness/` starts real nats-server
processes on the host for each test (details in NOTES.md).

```bash
python3 test-harness/remote-server-ctl.py --server hostbin/nats-server \
        --cwd build/nats.c-v3.15.0/test &
test-harness/run-unit-tests.sh            # optional 2nd arg: test-name regex
```

Last full run (QNX 8.0.0 x86_64 QEMU TCG, nats-server v2.15.3 on the host,
21 Nov 2026): **329 passed, 7 failed, 0 timed out**.

| Test | Why it failed |
|---|---|
| `natsSock_IPOrder` | IPv6-only (`::1` / `localhost` AAAA) is not redirected to the host; QEMU user-net IPv6 is not set up. |
| `IPResolutionOrder` | Same: `_startServer("", "-a :: -p 4222")` cannot be reached from the guest via the IPv4 harness. |
| `GetLocalAndRemoteIPAndPort` | Client (guest) and server (host) are not on the same host, so the reported addresses do not match what the test expects. |
| `natsTimer` | "Create performance" asserts many timers are created in &lt; 1 s; this misses under QEMU TCG (no KVM). |
| `AutoUnsubNoUnsubOnDestroy` | Harness flake: the test greps `server.log` after `_stopServer()`, and the log sometimes has not been shipped back yet. Passed when re-run on its own. |
| `HotSpotReconnect` | Many clients reconnecting through QEMU user-net SLIRP; timing, not a compile/link issue. Passed when re-run on its own. |
| `JetStreamOrderedConsumer` | Under TCG received 950/1024 chunks in the multi-loss case. Passed when re-run on its own after clock sync. |

Per-test logs and `results/summary.tsv` are in [`results/`](results/).

## Licence

Apache License 2.0 (same as nats.c) — see [LICENSE](LICENSE). The patches in
`patches/` modify nats.c, © The NATS Authors, Apache-2.0.
