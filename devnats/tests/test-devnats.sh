#!/bin/sh
# test-devnats.sh - automated functional test of the /dev/nats resource
# manager, run ON THE QNX GUEST as root.  TAP-style output.
# Needs a NATS server reachable from the guest (default nats://10.0.2.2:4222)
# with tools/host-echo running (svc.echo, svc.time) and a JetStream stream
# DEVNATS on 'devnats.js.>'.  devnats/tests/run-tests.sh sets all that up.
#   usage: test-devnats.sh [server-url]
BIN=${BIN:-/data/nats/bin}
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-/data/nats/lib}
export PATH=$BIN:$PATH
URL=${1:-nats://10.0.2.2:4222}
D=/dev/nats
T=/tmp/dntest.$$
mkdir -p $T
n=0; fail=0
ok()   { n=$((n+1)); echo "ok $n - $1"; }
nok()  { n=$((n+1)); fail=$((fail+1)); echo "not ok $n - $1"; [ -n "$2" ] && sed 's/^/#   /' "$2"; }
# expect NAME FILE PATTERN : pass if PATTERN (grep -E) found in FILE
expect() { if grep -Eq -- "$3" "$2"; then ok "$1"; else nok "$1" "$2"; fi; }
show() { sed 's/^/#   /' "$1"; }
start_dn() { slay -f devnats >/dev/null 2>&1; sleep 1
             nohup devnats -s "$URL" "$@" > $T/dn.log 2>&1 &
             i=0; while [ ! -d $D ] && [ $i -lt 10 ]; do sleep 1; i=$((i+1)); done; }

echo "# devnats functional tests on $(uname -a)"
echo "# server $URL, $(date -u)"

# --- 1. startup / attach ---------------------------------------------------
start_dn -v
sleep 1
if [ -d $D ]; then ok "devnats attaches $D"; else nok "devnats attaches $D" $T/dn.log; fi
expect "connects to server" $T/dn.log "connected to $URL"
expect "drops privileges after attach" $T/dn.log "privileges dropped"

# --- 2. text-mode write -> read --------------------------------------------
timeout 10 head -n 2 $D/qnx/demo > $T/r1 & P=$!; sleep 1
echo hello > $D/qnx/demo; printf 'line2\nline3\n' > $D/qnx/demo; wait $P
show $T/r1
if [ "$(cat $T/r1 | tr '\n' ' ')" = "hello line2 " ]; then ok "echo > / head < round trip, one message per line"; else nok "text round trip" $T/r1; fi

# --- 3. path mapping with dots ---------------------------------------------
timeout 10 head -n 1 $D/qnx/sensors.temp > $T/r2 & P=$!; sleep 1
echo 21.5 > $D/qnx/sensors/temp; wait $P
expect "/qnx/sensors/temp and /qnx/sensors.temp map to subject qnx.sensors.temp" $T/r2 '^21.5$'

# --- 4. wildcard subscribe; wildcard write rejected ------------------------
timeout 10 head -n 2 "$D/qnx/wild/>" > $T/r3 & P=$!; sleep 1
echo w1 > $D/qnx/wild/a; echo w2 > $D/qnx/wild/b/c; wait $P
show $T/r3
if [ "$(cat $T/r3 | tr '\n' ' ')" = "w1 w2 " ]; then ok "read on qnx.wild.> receives both subjects"; else nok "wildcard read" $T/r3; fi
if (echo x > "$D/qnx/wild/*") 2>$T/e1; then nok "write to wildcard rejected" $T/e1; else show $T/e1; ok "write to wildcard rejected"; fi
if (echo x > "$D/bad/>/mid") 2>$T/e2; then nok "'>' not last is rejected" $T/e2; else ok "'>' not last is rejected"; fi

# --- 5. O_NONBLOCK ----------------------------------------------------------
nats-ctl nbread $D/qnx/empty > $T/r4; show $T/r4
expect "O_NONBLOCK read on empty queue -> EAGAIN" $T/r4 'errno=EAGAIN'

# --- 6. blocking read interrupted by a signal (io_unblock) ------------------
nats-ctl read $D/qnx/never 1 2 > $T/r5; show $T/r5
expect "blocked read unblocked by SIGALRM -> EINTR" $T/r5 'Interrupted'

# --- 7. select() and poll() -------------------------------------------------
nats-ctl select $D/qnx/sel 300 > $T/r6; expect "select() times out with no data" $T/r6 'select: timeout'
nats-ctl select $D/qnx/sel 5000 > $T/r7 & P=$!; sleep 1; echo sel-msg > $D/qnx/sel; wait $P; show $T/r7
expect "select() wakes on publish" $T/r7 'select: readable, got sel-msg'
nats-ctl poll $D/qnx/pol 5000 > $T/r8 & P=$!; sleep 1; echo poll-msg > $D/qnx/pol; wait $P; show $T/r8
expect "poll() wakes on publish" $T/r8 'poll: readable, got poll-msg'

# --- 8. ionotify + pulse client --------------------------------------------
nats-notify -p 30 -n 3 $D/qnx/pulse > $T/r9 & P=$!; sleep 1
echo p1 > $D/qnx/pulse; printf 'p2\np3\n' > $D/qnx/pulse; sleep 2; kill $P 2>/dev/null; wait $P 2>/dev/null; show $T/r9
expect "ionotify pulse client receives 3 messages (SCHED_FIFO 30)" $T/r9 'received 3 message'

# --- 9. bounded queue + drop counter ---------------------------------------
nats-ctl flood $D/qnx/flood 4 3000 > $T/r10 & P=$!; sleep 1
for i in 1 2 3 4 5 6 7 8 9 10; do echo m$i; done > $D/qnx/flood; wait $P; show $T/r10
expect "qmax=4 with 10 messages -> 4 queued, 6 dropped" $T/r10 'in=10 .*drops=6 queued=4 qmax=4'

# --- 10. stats --------------------------------------------------------------
nats-ctl stats $D > $T/r11; show $T/r11
expect "DCMD_NATS_GET_STATS reports connection" $T/r11 "server=$URL connected=1"

# --- 11. request/reply ------------------------------------------------------
nats-ctl req $D/svc/echo "hi from qnx" 3000 > $T/r12; show $T/r12
expect "DCMD_NATS_REQUEST svc.echo" $T/r12 'reply: echo: hi from qnx'
nats-ctl req $D/svc/time "" 3000 > $T/r13; show $T/r13
expect "DCMD_NATS_REQUEST svc.time" $T/r13 'reply: 20[0-9-]+T'
nats-ctl req $D/svc/nobody "x" 500 > $T/r14 2>&1; show $T/r14
expect "request with no responder fails cleanly" $T/r14 'request failed: no responders'

# --- 12. JetStream publish with ack ----------------------------------------
ID=dn-$$-$(date +%s)
nats-ctl jspub $D/devnats/js/test "persist me" $ID > $T/r15
nats-ctl jspub $D/devnats/js/test "persist me" $ID >> $T/r15
nats-ctl jspub $D/nostream/x "lost" >> $T/r15; show $T/r15
expect "DCMD_NATS_JS_PUBLISH acked by stream DEVNATS" $T/r15 'stream=DEVNATS seq=[0-9]+ duplicate=0'
expect "same Msg-Id is reported as duplicate" $T/r15 'duplicate=1'
expect "JS publish to a subject with no stream fails (ENOENT)" $T/r15 "jspub failed: no stream"

# --- 13. queue group --------------------------------------------------------
nats-ctl qsub $D/work/q workers 8 4 > $T/q1 & P1=$!
nats-ctl qsub $D/work/q workers 8 4 > $T/q2 & P2=$!; sleep 1
for i in 1 2 3 4 5 6 7 8; do echo job$i; done > $D/work/q; wait $P1 $P2
a=$(grep -c '\[workers\]' $T/q1); b=$(grep -c '\[workers\]' $T/q2)
echo "#   member1=$a member2=$b"
if [ $((a+b)) -eq 8 ] && [ $a -gt 0 ] && [ $b -gt 0 ]; then ok "queue group splits 8 jobs across 2 members (each once)"; else nok "queue group"; cat $T/q1 $T/q2; fi

# --- 14. slog2 --------------------------------------------------------------
slog2info -b devnats > $T/r16 2>&1; tail -3 $T/r16 | sed 's/^/#   /'
expect "slog2 buffer 'devnats' has log lines" $T/r16 'attached /dev/nats'

# --- 15. SIGTERM: readers get EOF, path disappears --------------------------
cat $D/qnx/eof > $T/r17 & P=$!; sleep 1
slay -s TERM devnats >/dev/null; wait $P; rc=$?; sleep 1
if [ $rc -eq 0 ]; then ok "SIGTERM: blocked cat gets EOF (rc 0)"; else nok "SIGTERM EOF rc=$rc"; fi
if [ -d $D ]; then nok "path removed on shutdown"; else ok "path removed on shutdown"; fi
expect "clean shutdown logged" $T/dn.log 'stopped: in='

# --- 16. -U privilege drop + raw mode --------------------------------------
start_dn -U qnxuser -r
sleep 1
pidin -f anU 2>/dev/null | grep devnats > $T/r18; show $T/r18
expect "-U qnxuser: daemon runs as uid 1000" $T/r18 ' 1000 *$'
nats-ctl read $D/raw/y 1 5 > $T/r19 & P=$!; sleep 1; printf 'r1\nr2' > $D/raw/y; wait $P
if [ "$(sed -n 2,3p $T/r19 | tr '\n' '|')" = "r1|r2" ]; then ok "raw mode: one write = one message, no newline framing"; else nok "raw mode" $T/r19; fi
slay -s TERM devnats >/dev/null; sleep 1

rm -rf $T
echo "1..$n"
echo "# $((n-fail))/$n passed"
[ $fail -eq 0 ]
