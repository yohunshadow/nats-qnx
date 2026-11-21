#!/usr/bin/env python3
"""Drive real PTY / tmux / SSH sessions inside asciinema rec with natural typing."""
import fcntl, os, pty, random, re, select, struct, subprocess, sys, termios, time

SSH_CMD = (
    "ssh -tt -q -p 2222 -i /workspace/qnx-qemu/qnx_qemu_key "
    "-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "
    "-o LogLevel=ERROR root@127.0.0.1"
)

def open_qnx_cmd():
    return (
        f'{SSH_CMD} "export TERM=xterm-256color LD_LIBRARY_PATH=/data/nats/lib '
        f'PATH=/data/nats/bin:\\$PATH; '
        f"PS1=\\$(printf '\\\\033[1;36mqnx8#\\\\033[0m '); export PS1; exec sh -i\""
    )

def open_host_cmd():
    return (
        "env TERM=xterm-256color PATH=/workspace/nats-qnx/hostbin:$PATH "
        "NATS_URL=nats://127.0.0.1:4222 "
        "PS1=$'\\[\\e[1;32m\\]host$\\[\\e[0m\\] ' bash --norc --noprofile -i"
    )

def strip_ansi(s):
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)

def run_pty(cmd, script_fn, cols=104, rows=26):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.execvp("bash", ["bash", "-lc", cmd])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    buf = ""
    echo = False

    def drain(timeout=0.05):
        nonlocal buf
        end = time.monotonic() + timeout
        while True:
            rem = end - time.monotonic()
            if rem <= 0:
                break
            r, _, _ = select.select([fd], [], [], rem)
            if not r:
                break
            try:
                data = os.read(fd, 4096)
            except OSError:
                break
            if not data:
                break
            txt = data.decode("utf-8", errors="replace")
            buf += txt
            if echo:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()

    def wait_for(pattern, timeout=15.0):
        nonlocal buf
        rx = re.compile(pattern)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            clean = strip_ansi(buf)
            if rx.search(clean):
                buf = ""
                return True
            drain(0.03)
        raise TimeoutError(f"Timed out waiting for {pattern!r}; tail={strip_ansi(buf)[-300:]!r}")

    def type_paced(text, mean=0.038):
        for ch in text:
            os.write(fd, ch.encode())
            drain(max(0.016, random.gauss(mean, mean * 0.30)))
        drain(0.10)
        os.write(fd, b"\r")

    class Session:
        def wait(self, pat, timeout=15.0):
            return wait_for(pat, timeout)
        def show(self):
            nonlocal echo, buf
            echo = True
            buf = ""
        def hide(self):
            nonlocal echo
            echo = False
        def type(self, s, mean=0.038):
            type_paced(s, mean)
        def sleep(self, secs):
            drain(secs)
        def send_raw(self, b):
            os.write(fd, b)

    try:
        script_fn(Session())
    finally:
        try:
            os.close(fd)
        except OSError:
            pass
        try:
            os.waitpid(pid, 0)
        except OSError:
            pass

def tmux(*args):
    return subprocess.run(["tmux", *args], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True).stdout

def tmux_wait(pane, pattern, timeout=15.0):
    rx = re.compile(pattern)
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        txt = tmux("capture-pane", "-t", pane, "-p")
        if rx.search(txt):
            return txt
        time.sleep(0.04)
    raise TimeoutError(f"tmux wait {pattern!r} on {pane} timed out:\n{tmux('capture-pane', '-t', pane, '-p')}")

def tmux_type(pane, text, mean=0.038, enter=True):
    tmux("select-pane", "-t", pane)
    for ch in text:
        tmux("send-keys", "-t", pane, "-l", ch)
        time.sleep(max(0.016, random.gauss(mean, mean * 0.28)))
    if enter:
        time.sleep(0.10)
        tmux("send-keys", "-t", pane, "Enter")

def tmux_clear(pane, prompt_pat):
    tmux("send-keys", "-t", pane, "clear", "Enter")
    rx = re.compile(prompt_pat)
    end = time.monotonic() + 5.0
    while time.monotonic() < end:
        lines = [l for l in tmux("capture-pane", "-t", pane, "-p").splitlines() if l.strip()]
        if len(lines) == 1 and rx.search(lines[0]):
            return
        time.sleep(0.05)
