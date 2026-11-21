#!/usr/bin/env python3
import os, sys
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
        s.type("nats-lat -m poll -w /dev/nats/lat/local -r /dev/nats/lat/local -n 200 -i 5000 -P 1000")
        s.wait(r"qnx8#")
        s.sleep(0.30)
        s.type("nats-lat -m pulse -w /dev/nats/lat/local -r /dev/nats/lat/local -n 200 -i 5000")
        s.wait(r"qnx8#")
        s.sleep(0.30)
        s.type("nats-lat -m block -w /dev/nats/lat/local -r /dev/nats/lat/local -n 200 -i 5000")
        s.wait(r"qnx8#")
        s.sleep(0.30)
        s.type("nats-lat -m pulse -w /dev/nats/lat/ping -r /dev/nats/lat/pong -n 200 -i 5000")
        s.wait(r"qnx8#")
        s.sleep(1.3)
        s.hide()
        s.send_raw(b"exit\r")
        s.sleep(0.2)

    run_pty(open_qnx_cmd(), run, cols=132, rows=25)

if __name__ == "__main__":
    main()
