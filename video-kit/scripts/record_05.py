#!/usr/bin/env python3
import os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from rec_lib import open_qnx_cmd, run_pty

def main():
    def run(s):
        s.wait(r"qnx8#")
        s.send_raw(b"clear\r")
        s.wait(r"qnx8#")
        s.show()
        s.send_raw(b"\r")
        s.wait(r"qnx8#")
        s.sleep(0.30)
        s.type('nats-ctl req /dev/nats/svc/echo "ping from QNX" 2000')
        s.wait(r"qnx8#")
        s.sleep(0.45)
        s.type('nats-ctl req /dev/nats/svc/time "" 2000')
        s.wait(r"qnx8#")
        s.sleep(0.45)
        mid = f"tel-{int(time.time()) % 10000}"
        s.type(f"nats-ctl jspub /dev/nats/devnats/js/demo '{{\"temp\":24.5}}' {mid}")
        s.wait(r"qnx8#")
        s.sleep(0.45)
        s.type(f"nats-ctl jspub /dev/nats/devnats/js/demo '{{\"temp\":24.5}}' {mid}")
        s.wait(r"qnx8#")
        s.sleep(0.45)
        s.type("nats-ctl stats /dev/nats")
        s.wait(r"qnx8#")
        s.sleep(1.4)
        s.hide()
        s.send_raw(b"exit\r")
        s.sleep(0.2)

    run_pty(open_qnx_cmd(), run, cols=104, rows=25)

if __name__ == "__main__":
    main()
