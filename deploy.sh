#!/bin/bash
# deploy.sh - copy libnats + examples + qnx-smoke (+ test-suite & harness) to
# the QNX QEMU guest over SSH.  Usage: ./deploy.sh [x86_64]
set -euo pipefail
cd "$(dirname "$0")"
ARCH=${1:-x86_64}
GUEST_DIR=${GUEST_DIR:-/data/nats}
SRC=$(ls -d build/nats.c-*/ | head -1)
SSH_KEY=${SSH_KEY:-/workspace/qnx-qemu/qnx_qemu_key}
SSH="ssh -p ${SSH_PORT:-2222} -i $SSH_KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@${SSH_HOST:-127.0.0.1}"
STAGE=$(mktemp -d)
mkdir -p "$STAGE/lib" "$STAGE/bin" "$STAGE/t"
cp -P out/$ARCH/lib/libnats.so* "$STAGE/lib/"
cp out/$ARCH/bin/* "$STAGE/bin/"
cp out/$ARCH/test-harness/* "$STAGE/bin/"
mkdir -p "$STAGE/t/test" && cp -r "$SRC"/test/certs "$SRC"/test/*.conf "$SRC"/test/*.txt "$STAGE/t/test/"
$SSH "mkdir -p $GUEST_DIR"
tar cz -C "$STAGE" . | $SSH "cd $GUEST_DIR && gzip -dc | tar xf -"
rm -rf "$STAGE"
$SSH "ls -l $GUEST_DIR/lib $GUEST_DIR/bin | head -40"
