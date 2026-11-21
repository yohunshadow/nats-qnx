#!/usr/bin/env python3
"""remote-server-ctl.py - host side of the nats-server-remote shim.

Runs on the dev host. For every connection from the QNX target's
`nats-server-remote` shim it starts a real `nats-server` with the same
arguments (cwd = the upstream nats.c test/ directory, so relative cert and
config paths resolve), reports READY / EXITED, and kills the server when the
shim sends STOP (i.e. when the test calls _stopServer()).

Usage: remote-server-ctl.py --server ./hostbin/nats-server --cwd <nats.c>/test
Test-only tooling. Licensed under the Apache License 2.0.
"""
import argparse, os, socket, subprocess, threading, time, sys

def handle(conn, addr, server, cwd, log):
    f = conn.makefile("rb")
    args = []
    try:
        hdr = f.readline().decode().strip()
        n = int(hdr.split()[1])
        for _ in range(n):
            args.append(f.readline().decode().rstrip("\n")[4:])
        while True:
            line = f.readline().decode()
            if not line or line.strip() == "GO":
                break
            if line.startswith("FILE "):
                _, ln, path = line.rstrip("\n").split(" ", 2)
                data = f.read(int(ln))
                dst = path if os.path.isabs(path) else os.path.join(cwd, path)
                os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
                with open(dst, "wb") as out:
                    out.write(data)
        log(f"START {' '.join(args)}")
        p = subprocess.Popen([server] + args, cwd=cwd,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.15)
        if p.poll() is not None:
            conn.sendall(f"EXITED {p.returncode}\n".encode())
            log(f"EXITED early rc={p.returncode} {' '.join(args)}")
            return
        conn.sendall(b"READY\n")
        # watch for STOP or server exit
        conn.settimeout(0.1)
        while True:
            try:
                d = conn.recv(64)
                if not d or b"STOP" in d:
                    break
            except socket.timeout:
                pass
            if p.poll() is not None:
                conn.sendall(f"EXITED {p.returncode}\n".encode())
                return
        p.terminate()
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
        # Ship the server log back (tests such as AutoUnsubNoUnsubOnDestroy
        # read "-l server.log" after stopping the server).
        payload = b""
        if "-l" in args and args.index("-l") + 1 < len(args):
            lp = args[args.index("-l") + 1]
            lp = lp if os.path.isabs(lp) else os.path.join(cwd, lp)
            try:
                with open(lp, "rb") as fh:
                    payload = fh.read()
            except OSError:
                pass
        try:
            conn.sendall(f"STOPPED {len(payload)}\n".encode() + payload)
        except OSError:
            pass
        log(f"STOPPED {' '.join(args)}")
    except Exception as e:  # noqa
        log(f"error: {e!r}")
    finally:
        try:
            if 'p' in locals() and p.poll() is None:
                p.kill(); p.wait()
        except Exception:
            pass
        conn.close()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--cwd", required=True)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=4299)
    a = ap.parse_args()
    lk = threading.Lock()
    def log(m):
        with lk:
            print(time.strftime("%H:%M:%S"), m, flush=True)
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((a.bind, a.port)); s.listen(64)
    log(f"listening on {a.bind}:{a.port}")
    while True:
        c, addr = s.accept()
        threading.Thread(target=handle, args=(c, addr, os.path.abspath(a.server), a.cwd, log), daemon=True).start()

if __name__ == "__main__":
    main()
