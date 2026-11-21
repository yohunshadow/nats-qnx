#!/usr/bin/env python3
import os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from rec_lib import open_host_cmd, open_qnx_cmd, tmux, tmux_clear, tmux_type, tmux_wait

SESS = "natsvid3"
CONF = os.path.join(os.path.dirname(__file__), "tmux.conf")
CAST = os.path.join(os.path.dirname(__file__), "..", "casts", "03-host-to-guest.cast")

def main():
    subprocess.run(["tmux", "kill-session", "-t", SESS], stderr=subprocess.DEVNULL)
    tmux("-f", CONF, "new-session", "-d", "-s", SESS, "-x", "128", "-y", "26", open_host_cmd())
    tmux("split-window", "-h", "-t", f"{SESS}:0.0", open_qnx_cmd())
    tmux("select-pane", "-t", f"{SESS}:0.0", "-T", "Linux Host (nats CLI)")
    tmux("select-pane", "-t", f"{SESS}:0.1", "-T", "QNX 8.0 Guest (/dev/nats/host/cmd)")
    tmux_wait(f"{SESS}:0.0", r"host\$")
    tmux_wait(f"{SESS}:0.1", r"qnx8#")
    tmux("send-keys", "-t", f"{SESS}:0.1", "slay -f cat >/dev/null 2>&1; true", "Enter")
    tmux_wait(f"{SESS}:0.1", r"qnx8#")
    tmux_clear(f"{SESS}:0.0", r"host\$")
    tmux_clear(f"{SESS}:0.1", r"qnx8#")
    tmux("select-pane", "-t", f"{SESS}:0.1")

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
        tmux_type(f"{SESS}:0.1", "cat /dev/nats/host/cmd")
        time.sleep(0.5)
        tmux_type(f"{SESS}:0.0", 'nats pub host.cmd "start motor"')
        tmux_wait(f"{SESS}:0.1", r"start motor")
        time.sleep(0.45)
        tmux_type(f"{SESS}:0.0", 'nats pub host.cmd "set rpm 1800"')
        tmux_wait(f"{SESS}:0.1", r"set rpm 1800")
        time.sleep(0.45)
        tmux_type(f"{SESS}:0.0", 'nats pub host.cmd "stop motor"')
        tmux_wait(f"{SESS}:0.1", r"stop motor")
        time.sleep(1.4)
    finally:
        open(flag, "w").close()
        time.sleep(0.15)
        subprocess.run(["tmux", "kill-session", "-t", SESS], stderr=subprocess.DEVNULL)
        rec.wait(timeout=10)

if __name__ == "__main__":
    main()
