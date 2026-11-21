#!/bin/bash
# host-nats-server.sh - download nats-server (+ nats CLI) for the Linux dev
# host and run it on 0.0.0.0:4222 with JetStream, reachable from the QEMU
# guest as 10.0.2.2:4222.
# Usage: scripts/host-nats-server.sh [start|stop]
set -euo pipefail
cd "$(dirname "$0")/.."
NATS_SERVER_VERSION=${NATS_SERVER_VERSION:-v2.15.3}
NATS_CLI_VERSION=${NATS_CLI_VERSION:-v0.5.2}
PORT=${PORT:-4222}
STORE=${STORE:-/tmp/nats-qnx-js}
mkdir -p hostbin
if [ ! -x hostbin/nats-server ]; then
  curl -sSL "https://github.com/nats-io/nats-server/releases/download/$NATS_SERVER_VERSION/nats-server-$NATS_SERVER_VERSION-linux-amd64.tar.gz" \
    | tar xz -C hostbin --strip-components=1 "nats-server-$NATS_SERVER_VERSION-linux-amd64/nats-server"
fi
if [ ! -x hostbin/nats ]; then
  tmp=$(mktemp -d)
  curl -sSL -o "$tmp/n.zip" "https://github.com/nats-io/natscli/releases/download/$NATS_CLI_VERSION/nats-${NATS_CLI_VERSION#v}-linux-amd64.zip"
  unzip -q -o -j "$tmp/n.zip" "*/nats" -d hostbin && rm -rf "$tmp"
fi
case ${1:-start} in
  start)
    mkdir -p "$STORE"
    nohup hostbin/nats-server -a 0.0.0.0 -p "$PORT" -js -sd "$STORE" > /tmp/nats-qnx-server.log 2>&1 &
    echo $! > /tmp/nats-qnx-server.pid
    sleep 1; tail -2 /tmp/nats-qnx-server.log ;;
  stop)
    kill "$(cat /tmp/nats-qnx-server.pid)" 2>/dev/null || true ;;
esac
