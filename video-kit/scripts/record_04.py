#!/usr/bin/env python3
import os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from rec_lib import open_qnx_cmd, tmux, tmux_clear, tmux_type, tmux_wait

SESS = "natsvid4"
CONF = os.path.join(os.path.dirname(__file__), "tmux.conf")
CAST = os.path.join(os.path.dirname(__file__), "..", "casts", "04-ionotify-pulse.cast")

def main():
    subprocess.run(["tmux", "kill-session", "-t", SESS], stderr=subprocess.DEVNULL)
    tmux("-f", CONF, "new-session", "-d", "-s", SESS, "-x", "128", "-y", "26", open_qnx_cmd())
    tmux("split-window", "-h", "-t", f"{SESS}:0.0", open_qnx_cmd())
    tmux("select-pane", "-t", f"{SESS}:0.0", "-T", "QNX 8.0 :: ionotify + MsgReceivePulse (SCHED_FIFO 30)")
    tmux("select-pane", "-t", f"{SESS}:0.1", "-T", "QNX 8.0 :: shell publisher")
    tmux_wait(f"{SESS}:0.0", r"qnx8#")
    tmux_wait(f"{SESS}:0.1", r"qnx8#")
    tmux_clear(f"{SESS}:0.0", r"qnx8#")
    tmux_clear(f"{SESS}:0.1", r"qnx8#")
    tmux("select-pane", "-t", f"{SESS}:0.0")

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
        tmux_type(f"{SESS}:0.0", "nats-notify -p 30 -n 4 /dev/nats/qnx/pulse")
        tmux_wait(f"{SESS}:0.0", r"waiting on /dev/nats/qnx/pulse")
        time.sleep(0.55)
        tmux_type(f"{SESS}:0.1", 'echo "brake=42bar" > /dev/nats/qnx/pulse')
        tmux_wait(f"{SESS}:0.0", r"brake=42bar")
        time.sleep(0.45)
        tmux_type(f"{SESS}:0.1", 'printf "steer=12.4\\nthrottle=85\\n" > /dev/nats/qnx/pulse')
        tmux_wait(f"{SESS}:0.0", r"throttle=85")
        time.sleep(0.45)
        tmux_type(f"{SESS}:0.1", 'echo "gear=D" > /dev/nats/qnx/pulse')
        tmux_wait(f"{SESS}:0.0", r"received 4 message")
        time.sleep(1.4)
    finally:
        open(flag, "w").close()
        time.sleep(0.15)
        subprocess.run(["tmux", "kill-session", "-t", SESS], stderr=subprocess.DEVNULL)
        rec.wait(timeout=10)

if __name__ == "__main__":
    main()
