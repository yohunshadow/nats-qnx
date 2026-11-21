#!/usr/bin/env python3
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from rec_lib import open_qnx_cmd, run_pty

def main():
    def run(s):
        s.wait(r"qnx8#")
        s.send_raw(b"slay -f devnats >/dev/null 2>&1; clear\r")
        s.wait(r"qnx8#")
        s.show()
        s.send_raw(b"\r")
        s.wait(r"qnx8#")
        s.sleep(0.30)
        s.type("uname -a")
        s.wait(r"qnx8#")
        s.sleep(0.45)
        s.type("ls -lh /data/nats/bin/devnats /data/nats/lib/libnats.so.3.15.0")
        s.wait(r"qnx8#")
        s.sleep(0.45)
        s.type("devnats -v &")
        s.wait(r"privileges dropped")
        s.sleep(0.35)
        s.send_raw(b"\r")
        s.wait(r"qnx8#")
        s.sleep(0.35)
        s.type("ls -ld /dev/nats")
        s.wait(r"qnx8#")
        s.sleep(0.45)
        s.type("pidin -f anU | grep devnats")
        s.wait(r"qnx8#")
        s.sleep(1.5)
        s.hide()
        s.send_raw(b"exit\r")
        s.sleep(0.2)

    run_pty(open_qnx_cmd(), run, cols=102, rows=24)

if __name__ == "__main__":
    main()
