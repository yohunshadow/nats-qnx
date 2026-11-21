# nats-qnx — Voiceover & Visual Script (~2:28 @ 150 wpm)

> **Audio / Pacing Rules for Animator:**
> * **Target duration:** ~2:25–2:35 (under 3:00 YouTube hackathon limit).
> * **Voiceover rule (hard requirement from Oyen):** **NO pause longer than 2.0 seconds** anywhere in the narration. Keep transitions tight (0.3s–0.7s breathing room max between scenes).
> * **Tone:** Energetic, crisp, developer-to-developer — confident and aesthetic, not stiff or robotic.
> * **Repo note:** `github.com/yohunshadow/nats-qnx` is the **intended** URL and is **not yet live** — label any on-screen URL subtly or as `github.com/yohunshadow/nats-qnx` (Oyen will push before submission).

---

### Scene 1 — Hook: The Edge Messaging Gap (0:00 – 0:18)
* **Visual:** Motion-graphics opener — dark `#0d1117` / `#171b21` aesthetic. Stylized ECU / robot / medical device nodes on **QNX Neutrino 8.0** trying to talk to cloud services (`NATS` JetStream + edge brokers), without heavy protocol bridges. Kinetic title card: **`nats-qnx`** — *Native NATS & `/dev/nats` for QNX 8.0*.
* **Voiceover (41 words, ~16s):**
  > Safety-critical robots, vehicles, and medical devices run on QNX Neutrino, while the cloud and edge services around them increasingly speak NATS. Bridging those two worlds usually means writing custom gateways or brittle polling loops, so we brought native NATS messaging directly into QNX 8.0.

---

### Scene 2 — What `nats-qnx` Is & Booting `/dev/nats` (0:18 – 0:42)
* **Visual:** Brief clean 2-layer architecture card (`libnats.so.3.15.0` + OpenSSL 3.0.22 TLS at the base, `devnats` resource manager mounting `/dev/nats` above), cutting cleanly at 0:24 into **`clips/01-boot-devnats.mp4`** (9.97 s, can play 1.0× with clean zoom/framing on `devnats -v` and `/dev/nats`).
* **Voiceover (59 words, ~24s):**
  > **nats-qnx** works in two clean layers. First, we cross-compiled the official **nats.c** version 3.15 client library with OpenSSL 3 TLS using QNX's `qcc` toolchain. On top of that sits **devnats**, a tiny 39-kilobyte QNX resource manager that mounts **`/dev/nats`** straight into the OS path-space, logs via `slog2`, and immediately drops its privileges after attaching.

---

### Scene 3 — Live Demo: POSIX Shell Pub/Sub & Cloud-to-QNX Commands (0:42 – 1:10)
* **Visual:** Cut straight to **`clips/02-shell-pubsub.mp4`** (11.20 s, 3-pane live split: QNX `cat` + QNX `echo` + Linux `nats-server`), followed at 0:54 by **`clips/03-host-to-guest.mp4`** (8.63 s, Linux `nats pub` pushing motor commands live onto QNX `/dev/nats/host/cmd`). Highlight path-to-subject callout: `/dev/nats/qnx/sensors.temp` → `qnx.sensors.temp`.
* **Voiceover (68 words, ~27s):**
  > Because `/dev/nats` is a real QNX resource manager, any shell script or legacy C app can speak NATS using plain POSIX files. Writing a line to `/dev/nats/qnx/demo` publishes straight to the NATS broker on our Linux host, while mapping dots or nested folders builds subjects automatically, complete with read-only wildcard support. In reverse, motor commands published from the host arrive immediately in a blocking `cat` on the QNX target.

---

### Scene 4 — The QNX Magic: Pulses, `devctl`, & JetStream (1:10 – 1:42)
* **Visual:** **`clips/04-ionotify-pulse.mp4`** (11.00 s, `SCHED_FIFO 30` pulse receiver waking on 4 telemetry events), followed at 1:23 by **`clips/05-devctl-jetstream.mp4`** (14.17 s, `nats-ctl req` + `jspub` showing `stream=DEVNATS`, `duplicate=0`, then `duplicate=1`). Overlay clean callouts: `_RESMGR_NOREPLY` + `MsgDeliverEvent()` and `DCMD_NATS_JS_PUBLISH`.
* **Voiceover (79 words, ~32s):**
  > Under the hood, **devnats** is built for real-time QNX idioms. Bounded per-descriptor queues drop oldest messages cleanly on overflow, and blocking readers never stall a thread-pool worker—they reply-block until the NATS callback answers directly. Real-time tasks register with `ionotify` and sleep in `MsgReceivePulse` at `SCHED_FIFO` priority, waking on arrival alongside full `select` and `poll` support. Through `devctl`, any descriptor can run request-reply, join queue groups, or publish to JetStream with durable acknowledgments and deduplication.

---

### Scene 5 — Latency Comparison: Pulses vs. Polling (1:42 – 2:06)
* **Visual:** **`clips/06-latency.mp4`** (19.37 s) alongside an animated horizontal bar chart comparing wake-up latency from `proof/proof-devnats.txt` (500 msgs, `SCHED_FIFO 20`):
  * Blocking `read()`: **89.7 µs** p50
  * `ionotify` + pulse: **205.2 µs** p50 (**~9.3× faster than polling**)
  * Polling loop (1-tick sleep): **1907.3 µs** p50
  * Include subtle lower-third badge: *Measured on QNX 8.0 QEMU TCG (software emulation, no KVM) — relative comparison*.
* **Voiceover (59 words, ~24s):**
  > Here is why native QNX event delivery matters. In our live benchmark on the QEMU guest, a polling loop spends nearly two milliseconds waiting on timer ticks at the median, whereas `ionotify` pulses wake a real-time reader in just two hundred microseconds—more than nine times faster with tight tail latency and zero wasted CPU cycles.

---

### Scene 6 — Verification, Upstream Fixes & Call to Action (2:06 – 2:30)
* **Visual:** Clean animated scorecard card + codelab preview:
  * **329 / 336** upstream `nats.c` unit tests passing on QNX 8.0 (0 timeouts)
  * **29 / 29** `devnats` resource manager guest tests passing
  * **5** upstream-ready patches (including two latent NULL-dereference fixes in `nats.c` error paths)
  * `x86_64` verified in QEMU + `aarch64le` cross-compiled + `docs/codelab.md`
  * End card: **`github.com/yohunshadow/nats-qnx`** • Apache-2.0 • *QNX Global Hackathon*
* **Voiceover (65 words, ~26s):**
  > Across the board, **329 of 336** upstream `nats.c` unit tests and all **29** automated `devnats` tests pass on QNX 8.0, backed by five clean patches that even fix two real NULL-pointer bugs in upstream error paths. Both `x86_64` and `aarch64` builds are ready today, complete with a step-by-step codelab so anyone can reproduce it. Check out **nats-qnx** on GitHub and bring NATS straight to `/dev/nats`.

---

## Script Timing Summary
* **Total spoken words:** **361 words** (excluding stage directions/headers).
* **Estimated narration time @ 150 wpm:** **2 min 24 sec** (144.4 s — right inside 2:20–2:40).
* **Max pause between beats:** **≤ 0.7 s** (strictly < 2.0 s throughout).
