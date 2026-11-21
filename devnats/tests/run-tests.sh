#!/bin/bash
# run-tests.sh - host-side runner for the devnats proof.
#  1. makes sure the host nats-server (JetStream, 0.0.0.0:4222), the DEVNATS
#     stream and tools/host-echo are up
#  2. deploys and runs devnats/tests/test-devnats.sh on the QNX guest (TAP)
#  3. shell demo + host <-> guest interop with the nats CLI
#  4. latency: ionotify pulse vs poll vs blocking read, guest->host->guest RTT
# Output goes to stdout; ./devnats/tests/run-tests.sh | tee proof/proof-devnats.txt
set -uo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
NATS="$ROOT/hostbin/nats -s nats://127.0.0.1:4222"
KEY=${SSH_KEY:-/workspace/qnx-qemu/qnx_qemu_key}
SSH="ssh -p ${SSH_PORT:-2222} -i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@${SSH_HOST:-127.0.0.1}"
G="export LD_LIBRARY_PATH=/data/nats/lib PATH=/data/nats/bin:\$PATH;"
hdr() { echo; echo "=================== $* ==================="; }

hdr "environment"
echo "host date: $(TZ=Asia/Jakarta date '+%Y-%m-%d %H:%M:%S WIB')"
echo "git: $(git log -1 --format='%h %s' 2>/dev/null)"
echo "nats-server: $($ROOT/hostbin/nats-server --version)"
scripts/host-nats-server.sh status 2>/dev/null || scripts/host-nats-server.sh start
$NATS stream info DEVNATS >/dev/null 2>&1 || \
  $NATS stream add DEVNATS --subjects 'devnats.js.>' --storage file --defaults >/dev/null
echo "stream DEVNATS: $($NATS stream info DEVNATS -j | python3 -c 'import json,sys;d=json.load(sys.stdin);print(d["config"]["subjects"], "msgs", d["state"]["messages"])')"
if ! pgrep -f hostbin/host-echo >/dev/null; then
  [ -x hostbin/host-echo ] || gcc -O2 -Iout/host/include -o hostbin/host-echo tools/host-echo.c out/host/lib/libnats_static.a -lpthread
  nohup hostbin/host-echo > /tmp/host-echo.log 2>&1 &
  sleep 1
fi
head -1 /tmp/host-echo.log
$SSH "date -u $(date -u +%m%d%H%M%Y.%S) >/dev/null; uname -a"
scp -q -P ${SSH_PORT:-2222} -i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
    out/x86_64/bin/{devnats,nats-ctl,nats-notify,nats-lat} devnats/tests/test-devnats.sh root@${SSH_HOST:-127.0.0.1}:/data/nats/bin/
$SSH "slay -f devnats nats-ctl nats-notify nats-lat >/dev/null 2>&1; true"

hdr "guest functional tests (devnats/tests/test-devnats.sh)"
timeout 300 $SSH "sh /data/nats/bin/test-devnats.sh"
FUNC_RC=$?

hdr "shell demo + interop with host nats CLI"
$SSH "$G nohup devnats -v > /tmp/devnats.out 2>&1 & sleep 3; cat /tmp/devnats.out"
echo '--- guest: cat /dev/nats/qnx/demo  |  guest: echo hello > /dev/nats/qnx/demo'
timeout 30 $SSH "$G (timeout 10 head -n 1 /dev/nats/qnx/demo > /tmp/demo.out &) ; sleep 1; echo hello > /dev/nats/qnx/demo; sleep 1; echo \"cat got: \$(cat /tmp/demo.out)\""
echo '--- guest -> host: host runs  nats sub "qnx.>" ; guest writes'
timeout 20 $NATS sub 'qnx.>' --count 2 > /tmp/hostsub.out 2>&1 &
HS=$!; sleep 1
$SSH "echo 'from qnx shell' > /dev/nats/qnx/demo; echo 'via dots' > /dev/nats/qnx/sensors.temp"
wait $HS; grep -E 'Received|from qnx|via dots' /tmp/hostsub.out
echo '--- host -> guest: guest reads /dev/nats/host/cmd ; host runs  nats pub host.cmd'
( timeout 20 $SSH "$G timeout 15 head -n 2 /dev/nats/host/cmd" > /tmp/guestread.out 2>&1 ) &
GR=$!; sleep 3
$NATS pub host.cmd "start motor" 2>&1; $NATS pub host.cmd "stop motor" 2>&1
wait $GR; sed 's/^/guest read: /' /tmp/guestread.out
echo '--- guest devctl request -> host service'
$SSH "$G nats-ctl req /dev/nats/svc/time '' 2000"
echo '--- stats'
$SSH "$G nats-ctl stats /dev/nats"

hdr "latency (QEMU TCG, no KVM: numbers are emulation-bound, not QNX-on-silicon)"
N=${LAT_N:-500}
for m in pulse poll block; do
  echo "--- guest-local, reader mode $m (write and read /dev/nats/lat/local, via host server)"
  timeout 120 $SSH "$G nats-lat -m $m -w /dev/nats/lat/local -r /dev/nats/lat/local -n $N -i 5000 -P 1000"
done
echo "--- guest -> host echo -> guest round trip (lat.ping -> host-echo -> lat.pong), pulse reader"
timeout 120 $SSH "$G nats-lat -m pulse -w /dev/nats/lat/ping -r /dev/nats/lat/pong -n $N -i 5000"
echo "--- devctl request/reply round trip (svc.echo, 20 sequential)"
$SSH "$G i=0; s=\$(date +%s); while [ \$i -lt 20 ]; do nats-ctl req /dev/nats/svc/echo x 2000 >/dev/null || echo fail; i=\$((i+1)); done; echo \"20 requests incl. process spawn: \$((\$(date +%s)-s)) s\""
$SSH "slay -s TERM devnats >/dev/null 2>&1; sleep 1; tail -1 /tmp/devnats.out"

hdr "result"
if [ $FUNC_RC -eq 0 ]; then echo "functional tests: PASS"; else echo "functional tests: FAIL ($FUNC_RC)"; fi
exit $FUNC_RC
