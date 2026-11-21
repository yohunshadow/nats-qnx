#!/bin/bash
# build.sh - fetch upstream nats.c, apply QNX patches, cross-compile for QNX 8.0.
#
# Usage: ./build.sh [x86_64|aarch64le|all]   (default: x86_64)
# Env:   QNX_SDP      path to SDP 8.0       (default: $HOME/qnx800)
#        NATS_C_TAG   upstream tag          (default: v3.15.0)
#        NATS_TLS     ON|OFF  OpenSSL TLS   (default: ON; OFF = no OpenSSL dep)
#        BUILD_TESTS  ON|OFF  upstream test-suite (default: ON)
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
QNX_SDP=${QNX_SDP:-$HOME/qnx800}
NATS_C_TAG=${NATS_C_TAG:-v3.15.0}
NATS_TLS=${NATS_TLS:-ON}
BUILD_TESTS=${BUILD_TESTS:-ON}
ARCHES=${1:-x86_64}
[ "$ARCHES" = all ] && ARCHES="x86_64 aarch64le"

# shellcheck disable=SC1091
source "$QNX_SDP/qnxsdp-env.sh" >/dev/null

SRC=$ROOT/build/nats.c-$NATS_C_TAG
if [ ! -d "$SRC" ]; then
  mkdir -p "$ROOT/build"
  git clone --quiet --depth 1 --branch "$NATS_C_TAG" https://github.com/nats-io/nats.c "$SRC"
  for p in "$ROOT"/patches/*.patch; do
    echo ">> applying $(basename "$p")"
    git -C "$SRC" apply --whitespace=nowarn "$p"
  done
fi

for ARCH in $ARCHES; do
  case $ARCH in
    x86_64)    V=gcc_ntox86_64 ;;
    aarch64le) V=gcc_ntoaarch64le ;;
    *) echo "unknown arch $ARCH"; exit 1 ;;
  esac
  B=$ROOT/build/$ARCH
  OUT=$ROOT/out/$ARCH
  echo ">> configuring nats.c $NATS_C_TAG for QNX $ARCH (TLS=$NATS_TLS)"
  cmake -S "$SRC" -B "$B" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/qnx-toolchain.cmake" -DQNX_ARCH="$ARCH" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$OUT" \
    -DNATS_BUILD_STREAMING=OFF -DNATS_BUILD_WITH_TLS="$NATS_TLS" \
    -DNATS_BUILD_EXAMPLES=ON -DBUILD_TESTING="$BUILD_TESTS" >/dev/null
  echo ">> building ($ARCH)"
  ninja -C "$B" >/dev/null
  ninja -C "$B" install >/dev/null
  mkdir -p "$OUT/bin" "$OUT/test-harness"
  cp "$B"/bin/nats-* "$OUT/bin/"
  [ -f "$B/bin/testsuite" ] && cp "$B/bin/testsuite" "$OUT/bin/"

  SSL_LIBS=""
  [ "$NATS_TLS" = ON ] && SSL_LIBS="-lssl -lcrypto"
  echo ">> building qnx-smoke + test harness ($ARCH)"
  qcc -V$V -O2 -Wall -I"$OUT/include" -o "$OUT/bin/qnx-smoke" "$ROOT/demo/qnx-smoke.c" \
      -L"$OUT/lib" -lnats $SSL_LIBS -lsocket
  qcc -V$V -O2 -Wall -o "$OUT/test-harness/nats-server-remote" \
      "$ROOT/test-harness/nats-server-remote.c" -lsocket
  qcc -V$V -O2 -Wall -shared -fPIC -o "$OUT/test-harness/loopback-redirect.so" \
      "$ROOT/test-harness/loopback-redirect.c" -lsocket
  echo ">> building devnats resource manager + clients ($ARCH)"
  DN_FLAGS="-O2 -Wall -Wextra -Wno-unused-parameter"
  qcc -V$V $DN_FLAGS -I"$OUT/include" -o "$OUT/bin/devnats" "$ROOT/devnats/devnats.c" \
      -L"$OUT/lib" -lnats $SSL_LIBS -lsocket -lslog2
  for ex in nats-notify nats-ctl nats-lat; do
    qcc -V$V $DN_FLAGS -o "$OUT/bin/$ex" "$ROOT/devnats/examples/$ex.c"
  done
  cp "$ROOT/devnats/devnats.h" "$OUT/include/nats/devnats.h"
  echo ">> done: $OUT"
  for f in "$OUT"/lib/libnats.so.*.*.* "$OUT/bin/qnx-smoke" "$OUT/bin/devnats"; do
    printf '   %-45s %s\n' "${f#$ROOT/}" "$(readelf -h "$f" | sed -n 's/.*Machine: *//p')"
  done
done
