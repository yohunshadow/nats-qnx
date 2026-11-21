#!/bin/bash
# run-proof.sh - capture end-to-end proof that nats.c works on the QNX 8.0
# QEMU guest against the host's nats-server (10.0.2.2:4222, JetStream on).
# Output goes to stdout and proof/proof-<arch>.txt.
set -uo pipefail
cd "$(dirname "$0")/.."
ARCH=${1:-x86_64}
G=${GUEST_DIR:-/data/nats}
URL=nats://10.0.2.2:4222
N=hostbin/nats
SSH="ssh -p ${SSH_PORT:-2222} -i ${SSH_KEY:-/workspace/qnx-qemu/qnx_qemu_key} -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@${SSH_HOST:-127.0.0.1}"
ENV="export LD_LIBRARY_PATH=$G/lib;"
mkdir -p proof
exec > >(tee "proof/proof-$ARCH.txt") 2>&1

section() { printf '\n==================== %s ====================\n' "$*"; }

section "environment"
echo "host: $(uname -srm), $(hostbin/nats-server --version)"
$SSH 'uname -a; echo "libnats: $(ls '$G'/lib/libnats.so.*.*.*)"'

section "1. core pub/sub on QNX (upstream nats-subscriber + nats-publisher)"
$SSH "$ENV ($G/bin/nats-subscriber -s $URL -subj qnx.demo -count 5 -print > /tmp/sub.out 2>&1 &); sleep 2;
      $G/bin/nats-publisher -s $URL -subj qnx.demo -txt hello-from-QNX-8.0 -count 5; sleep 2;
      echo '--- subscriber (QNX) ---'; grep -v 'In Msgs' /tmp/sub.out"

section "2. QNX -> Linux host (QNX publishes, host 'nats sub' receives)"
timeout 20 $N -s nats://127.0.0.1:4222 sub qnx.to.host --count 3 > /tmp/hostsub.out 2>&1 &
subpid=$!
sleep 1
$SSH "$ENV $G/bin/nats-publisher -s $URL -subj qnx.to.host -txt 'telemetry from QNX' -count 3 | tail -1"
wait $subpid || true
echo '--- host subscriber ---'; grep -E 'Received|telemetry' /tmp/hostsub.out || cat /tmp/hostsub.out

section "3. Linux host -> QNX (host 'nats pub', QNX nats-subscriber receives)"
$SSH "$ENV ($G/bin/nats-subscriber -s $URL -subj host.to.qnx -count 3 -print > /tmp/sub2.out 2>&1 &)"
sleep 2
$N -s nats://127.0.0.1:4222 pub host.to.qnx 'command {{Count}} from Linux host' --count 3
sleep 2
$SSH "echo '--- subscriber (QNX) ---'; grep Received /tmp/sub2.out"

section "4. JetStream on QNX (upstream nats-js-sub + nats-js-pub)"
$N -s nats://127.0.0.1:4222 stream rm QNXDEMO -f >/dev/null 2>&1
$SSH "$ENV ($G/bin/nats-js-sub -s $URL -stream QNXDEMO -subj qnx.js -count 10 -print > /tmp/jssub.out 2>&1 &); sleep 3;
      $G/bin/nats-js-pub -s $URL -stream QNXDEMO -subj qnx.js -txt js-hello-from-QNX -count 10 -sync"
# Capture stream state before js-sub exits and (if it created the stream) deletes it.
echo '--- host view of the stream ---'
$N -s nats://127.0.0.1:4222 stream info QNXDEMO -j 2>/dev/null | python3 -c 'import json,sys; d=json.load(sys.stdin); print("stream", d["config"]["name"], "subjects", d["config"]["subjects"], "messages", d["state"]["messages"], "last_seq", d["state"]["last_seq"])'
sleep 2
$SSH "echo '--- js-sub (QNX) ---'; grep -v 'In Msgs' /tmp/jssub.out"

section "5. qnx-smoke (pub/sub, request/reply, JetStream file stream + durable pull consumer, KV)"
$SSH "$ENV $G/bin/qnx-smoke $URL 5; echo exit=\$?"
echo '--- host view ---'
$N -s nats://127.0.0.1:4222 consumer info QNX_SMOKE qnx-smoke-durable -j 2>/dev/null | python3 -c 'import json,sys; d=json.load(sys.stdin); print("consumer", d["name"], "delivered", d["delivered"]["stream_seq"], "ack_floor", d["ack_floor"]["stream_seq"], "pending", d["num_pending"])'
$N -s nats://127.0.0.1:4222 kv get qnx_smoke_kv board.os --raw 2>/dev/null | sed 's/^/kv board.os from host: /'; echo

section "6. TLS (OpenSSL 3 on QNX, CA + hostname verification)"
SRC=$(ls -d build/nats.c-*/ | head -1)
( cd "$SRC/test" && exec ../../../hostbin/nats-server -a 0.0.0.0 -p 14443 --tls --tlscert certs/server-cert.pem --tlskey certs/server-key.pem ) > /tmp/tls-server.log 2>&1 &
TLSPID=$!
sleep 1
$SSH "$ENV cd $G/t/test; ($G/bin/nats-subscriber -s tls://10.0.2.2:14443 -tls -tlscacert certs/ca.pem -tlshost localhost -subj qnx.tls -count 3 -print > /tmp/tlssub.out 2>&1 &); sleep 2;
      $G/bin/nats-publisher -s tls://10.0.2.2:14443 -tls -tlscacert certs/ca.pem -tlshost localhost -subj qnx.tls -txt encrypted-hello-from-QNX -count 3 | tail -1; sleep 2;
      echo '--- TLS subscriber (QNX) ---'; grep Received /tmp/tlssub.out;
      echo '--- wrong CA must fail ---'; $G/bin/nats-publisher -s tls://10.0.2.2:14443 -tls -tlscacert certs/client-cert.pem -tlshost localhost -subj x -count 1 2>&1 | grep -iE 'error|fail' | head -2"
kill $TLSPID 2>/dev/null
echo; echo "proof written to proof/proof-$ARCH.txt"
