#!/usr/bin/env python3
import os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from rec_lib import open_host_cmd, open_qnx_cmd, tmux, tmux_clear, tmux_type, tmux_wait

SESS = "natsvid2"
CONF = os.path.join(os.path.dirname(__file__), "tmux.conf")
CAST = os.path.join(os.path.dirname(__file__), "..", "casts", "02-shell-pubsub.cast")

def main():
    subprocess.run(["tmux", "kill-session", "-t", SESS], stderr=subprocess.DEVNULL)
    tmux("-f", CONF, "new-session", "-d", "-s", SESS, "-x", "128", "-y", "26", open_qnx_cmd())
    tmux("split-window", "-h", "-t", f"{SESS}:0.0", open_host_cmd())
    tmux("split-window", "-v", "-t", f"{SESS}:0.0", "-l", "12", open_qnx_cmd())
    # Panes now: 0.0 top-left (QNX sub), 0.1 bottom-left (QNX pub), 0.2 right (Linux host)
    tmux("select-pane", "-t", f"{SESS}:0.0", "-T", "QNX 8.0 :: cat /dev/nats/qnx/demo")
    tmux("select-pane", "-t", f"{SESS}:0.1", "-T", "QNX 8.0 :: shell publisher")
    tmux("select-pane", "-t", f"{SESS}:0.2", "-T", "Linux Host :: nats-server (10.0.2.2:4222)")
    tmux_wait(f"{SESS}:0.0", r"qnx8#")
    tmux_wait(f"{SESS}:0.1", r"qnx8#")
    tmux_wait(f"{SESS}:0.2", r"host\$")
    tmux("send-keys", "-t", f"{SESS}:0.0", "slay -f cat >/dev/null 2>&1; true", "Enter")
    tmux_wait(f"{SESS}:0.0", r"qnx8#")
    tmux_clear(f"{SESS}:0.0", r"qnx8#")
    tmux_clear(f"{SESS}:0.1", r"qnx8#")
    tmux_clear(f"{SESS}:0.2", r"host\$")
    tmux("select-pane", "-t", f"{SESS}:0.0")
    time.sleep(0.25)

    flag = f"/tmp/{SESS}.done"
    if os.path.exists(flag): os.remove(flag)
    rec_py = os.path.join(os.path.dirname(__file__), "tmux_rec.py")
    env = {**os.environ, "TERM": "xterm-256color"}
    rec = subprocess.Popen([
        "asciinema", "rec", "--overwrite", "--idle-time-limit", "1.0",
        "--cols", "128", "--rows", "26", "-c", f"python3 {rec_py} {SESS} 128 26 {flag}", CAST
    ], env=env)
    try:
        time.sleep(0.5)
        tmux_type(f"{SESS}:0.0", "cat /dev/nats/qnx/demo")
        time.sleep(0.4)
        tmux_type(f"{SESS}:0.2", "nats sub 'qnx.>' --count 3")
        tmux_wait(f"{SESS}:0.2", r"Subscribing on qnx\.>")
        time.sleep(0.4)
        tmux_type(f"{SESS}:0.1", "echo hello > /dev/nats/qnx/demo")
        tmux_wait(f"{SESS}:0.0", r"hello")
        tmux_wait(f"{SESS}:0.2", r"hello")
        time.sleep(0.45)
        tmux_type(f"{SESS}:0.1", "echo 'temp=21.5C' > /dev/nats/qnx/sensors.temp")
        tmux_wait(f"{SESS}:0.2", r"qnx\.sensors\.temp")
        time.sleep(0.45)
        tmux_type(f"{SESS}:0.1", "echo 'status=ready' > /dev/nats/qnx/demo")
        tmux_wait(f"{SESS}:0.0", r"status=ready")
        tmux_wait(f"{SESS}:0.2", r"status=ready")
        time.sleep(1.5)
    finally:
        open(flag, "w").close()
        time.sleep(0.15)
        subprocess.run(["tmux", "kill-session", "-t", SESS], stderr=subprocess.DEVNULL)
        rec.wait(timeout=10)

if __name__ == "__main__":
    main()
