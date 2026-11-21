#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=/workspace/nats-qnx
KEY=/workspace/qnx-qemu/qnx_qemu_key
SSH="ssh -p 2222 -i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR root@127.0.0.1"

$ROOT/scripts/host-nats-server.sh status >/dev/null || $ROOT/scripts/host-nats-server.sh start
$ROOT/hostbin/nats -s nats://127.0.0.1:4222 stream info DEVNATS >/dev/null 2>&1 || \
  $ROOT/hostbin/nats -s nats://127.0.0.1:4222 stream add DEVNATS --subjects 'devnats.js.>' --storage file --defaults >/dev/null
if ! pgrep -f hostbin/host-echo >/dev/null; then
  nohup $ROOT/hostbin/host-echo > /tmp/host-echo.log 2>&1 &
  sleep 1
fi
$SSH "date -u $(date -u +%m%d%H%M%Y.%S) >/dev/null; slay -f devnats nats-ctl nats-notify nats-lat cat >/dev/null 2>&1; true"

echo "=== recording 01-boot-devnats ==="
TERM=xterm-256color asciinema rec --overwrite --idle-time-limit 1.0 --cols 102 --rows 24 -c "python3 scripts/record_01.py" casts/01-boot-devnats.cast

echo "=== ensuring nohup devnats on guest for clips 02-06 ==="
$SSH "slay -f devnats >/dev/null 2>&1; LD_LIBRARY_PATH=/data/nats/lib nohup /data/nats/bin/devnats >/tmp/devnats-vid.log 2>&1 & sleep 2; ls -d /dev/nats"

echo "=== recording 02-shell-pubsub ==="
python3 scripts/record_02.py

echo "=== recording 03-host-to-guest ==="
python3 scripts/record_03.py

echo "=== recording 04-ionotify-pulse ==="
python3 scripts/record_04.py

echo "=== recording 05-devctl-jetstream ==="
TERM=xterm-256color asciinema rec --overwrite --idle-time-limit 1.0 --cols 104 --rows 25 -c "python3 scripts/record_05.py" casts/05-devctl-jetstream.cast

echo "=== recording 06-latency ==="
TERM=xterm-256color asciinema rec --overwrite --idle-time-limit 1.0 --cols 108 --rows 25 -c "python3 scripts/record_06.py" casts/06-latency.cast

echo "=== rendering casts -> 1920x1080 MP4 ==="
render_one() {
  local name=$1 fsize=$2
  local cast="casts/${name}.cast"
  local gif="clips/${name}.gif"
  local mp4="clips/${name}.mp4"
  agg --quiet --theme github-dark \
      --text-font-family "Cascadia Code,IBM Plex Mono,DejaVu Sans Mono" \
      --font-size "$fsize" --line-height 1.35 \
      --fps-cap 30 --last-frame-duration 1.5 --idle-time-limit 1.0 \
      "$cast" "$gif"
  ffmpeg -y -loglevel error -i "$gif" \
      -vf "scale=1920:1080:force_original_aspect_ratio=decrease:flags=lanczos,pad=1920:1080:(ow-iw)/2:(oh-ih)/2:color=#171b21,fps=30,format=yuv420p" \
      -c:v libx264 -preset medium -crf 16 -movflags +faststart "$mp4"
  rm -f "$gif"
  local dur res sz
  dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$mp4")
  res=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$mp4")
  sz=$(du -h "$mp4" | cut -f1)
  printf "  %-24s %6.2fs  %s  %s\n" "${name}.mp4" "$dur" "$res" "$sz"
}

render_one 01-boot-devnats 26
render_one 02-shell-pubsub 22
render_one 03-host-to-guest 22
render_one 04-ionotify-pulse 22
render_one 05-devctl-jetstream 26
render_one 06-latency 25

$SSH "slay -s TERM devnats >/dev/null 2>&1; true"
pkill -f hostbin/host-echo 2>/dev/null || true
echo "=== done ==="
