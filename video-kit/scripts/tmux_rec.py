#!/usr/bin/env python3
"""Run `tmux attach -t <sess>` in a PTY with raw stdout so asciinema's PTY keeps tmux's exact cursor moves."""
import fcntl, os, pty, select, signal, struct, sys, termios, tty

sess, cols, rows, flag_file = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]

# Disable OPOST/ONLCR on our stdout (asciinema's PTY) so tmux's \n isn't turned into \r\n.
try:
    attrs = termios.tcgetattr(sys.stdout.fileno())
    attrs[1] &= ~termios.OPOST
    termios.tcsetattr(sys.stdout.fileno(), termios.TCSANOW, attrs)
except Exception:
    pass

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execvp("tmux", ["tmux", "attach", "-t", sess])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

while True:
    if os.path.exists(flag_file):
        break
    r, _, _ = select.select([fd], [], [], 0.03)
    if r:
        try:
            d = os.read(fd, 4096)
        except OSError:
            break
        if not d:
            break
        sys.stdout.buffer.write(d)
        sys.stdout.buffer.flush()

try:
    os.close(fd)
except OSError:
    pass
try:
    os.kill(pid, signal.SIGTERM)
    os.waitpid(pid, 0)
except OSError:
    pass
