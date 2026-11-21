# Unit-test results (QNX 8.0.0 x86_64 QEMU)

Last full run of the upstream nats.c v3.15.0 suite (`test/list_test.txt`,
336 tests) on the QNX guest, with real nats-server v2.15.3 processes on
the Linux host via `test-harness/`.

* Date: 21 Nov 2026, finished 03:54 WIB (Asia/Jakarta)
* Result: **329 passed, 7 failed, 0 timed out**
* Machine: QEMU TCG (`qemu-system-x86_64 -cpu max`, no KVM), 2 vCPU, 1 GB
* Guest: QNX Neutrino 8.0.0 (2026/10/30-09:10:05EDT)
* See `summary.tsv` and `logs/<TestName>.log`. The 7 failures are
  classified in the top-level README and NOTES.md.
