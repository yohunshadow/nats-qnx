#!/bin/bash
# Run the upstream nats.c unit tests on the QNX QEMU guest, one test per
# process (same as upstream ctest), with real nats-server processes on the host.
# Prereqs: testsuite + shims deployed to $GUEST_DIR (see README), and
# remote-server-ctl.py running on the host.
# Usage: run-unit-tests.sh [list_file] [filter-regex]
set -u
LIST=${1:-$(ls -d "$(dirname "$0")"/../build/nats.c-*/ | head -1)test/list_test.txt}
FILTER=${2:-.}
PER_TEST_TIMEOUT=${PER_TEST_TIMEOUT:-600}
GUEST_DIR=${GUEST_DIR:-/data/nats}
OUT=${OUT:-/workspace/nats-qnx/results}
SERVER_VERSION=${SERVER_VERSION:-$("$(dirname "$0")"/../hostbin/nats-server --version)}
SSH="ssh -p 2222 -i /workspace/qnx-qemu/qnx_qemu_key -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ServerAliveInterval=30 root@127.0.0.1"
mkdir -p "$OUT/logs"
: > "$OUT/summary.tsv"
pass=0; fail=0; to=0
for name in $(sed -n 's/^_test(\(.*\))$/\1/p' "$LIST" | grep -E "$FILTER"); do
  t0=$(date +%s)
  # QEMU/TCG guest clock drifts (~1 min/hour observed); JetStream tests
  # compare client timestamps with the server's, so resync before every test.
  # QNX `date` has 1 s resolution: wait until ~0.8 s into the current second
  # and set the *next* second, so the guest lands close to a second boundary.
  ns=$(date +%N); sleep "0.$(printf '%03d' $(( (1800 - 10#${ns:0:3}) % 1000 )))"
  timeout "$PER_TEST_TIMEOUT" $SSH "date -u $(date -u -d '+1 second' +%m%d%H%M%Y.%S) >/dev/null; cd $GUEST_DIR/t/test && export LD_LIBRARY_PATH=$GUEST_DIR/lib LD_PRELOAD=$GUEST_DIR/bin/loopback-redirect.so NATS_TEST_SERVER_EXE=$GUEST_DIR/bin/nats-server-remote NATS_TEST_SERVER_VERSION=\"$SERVER_VERSION\" && $GUEST_DIR/bin/testsuite $name" > "$OUT/logs/$name.log" 2>&1
  rc=$?
  dt=$(( $(date +%s) - t0 ))
  if [ $rc -eq 124 ]; then
    res=TIMEOUT; to=$((to+1))
    $SSH "slay -f testsuite; slay -f nats-server-remote" >/dev/null 2>&1
  elif grep -q "ALL PASSED" "$OUT/logs/$name.log"; then
    res=PASS; pass=$((pass+1))
  else
    res=FAIL; fail=$((fail+1))
  fi
  printf "%s\t%s\t%ss\n" "$name" "$res" "$dt" | tee -a "$OUT/summary.tsv"
done
echo "TOTAL pass=$pass fail=$fail timeout=$to" | tee -a "$OUT/summary.tsv"
