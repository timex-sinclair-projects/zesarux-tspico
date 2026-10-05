#!/usr/bin/env python3
"""The TS-Pico bridge in ZEsarUX against a stand-in: no TS-Pico firmware or
ROM needed, so CI can run it anywhere.

    python3 tspico/bridge_test.py src/zesarux

The stand-in listens on TCP, answers HELLO with version 1 and reads of port
0x0f with 0x42, and records every frame. ZEsarUX runs a TS2068 (stock ROM)
headless, with its remote protocol (ZRCP) on; over ZRCP the test stops the
CPU, writes

    9C40  F3          DI
          DB 0F       IN A,(0Fh)        a status read: op 2
          32 50 9C    LD (9C50h),A      where the bridge's 0x42 should land
          3E 55       LD A,55h
          D3 0E       OUT (0Eh),A       a data write: op 0, value 0x55
          18 FE       JR $

steps through it, and reads 9C50h back.
"""

import os
import re
import socket
import subprocess
import sys
import threading
import time

BRIDGE_PORT, ZRCP_PORT = 20681, 10681
STATUS = 0x42
frames = []


def bridge(srv):
    conn, _ = srv.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    buf = b""
    while True:
        try:
            got = conn.recv(64)
        except OSError:
            break
        if not got:
            break
        buf += got
        while len(buf) >= 2:
            op, value = buf[0], buf[1]
            buf = buf[2:]
            frames.append((op, value))
            conn.sendall(bytes([{4: 1, 2: STATUS}.get(op, 0)]))


class Zrcp:
    def __init__(self, port):
        t0 = time.time()
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                if time.time() - t0 > 30:
                    raise
                time.sleep(0.3)
        self.cmd("")

    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        buf = b""
        prompt = re.compile(rb"command(@cpu-step)?> ?$")   # the prompt changes in step mode
        while not prompt.search(buf):
            buf += self.s.recv(65536)
        return prompt.sub(b"", buf).decode(errors="replace").strip()


def main():
    emu = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "src/zesarux")
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", BRIDGE_PORT))
    srv.listen(1)
    threading.Thread(target=bridge, args=(srv,), daemon=True).start()

    env = dict(os.environ, TSPICO_BRIDGE="tcp:127.0.0.1:%d" % BRIDGE_PORT)
    p = subprocess.Popen([emu, "--machine", "TS2068", "--noconfigfile", "--vo", "null", "--ao", "null",
                          "--enable-remoteprotocol", "--remoteprotocol-port", str(ZRCP_PORT)],
                         cwd=os.path.dirname(emu), env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    state = ""
    try:
        time.sleep(2)                                   # let the ROM come up
        z = Zrcp(ZRCP_PORT)
        z.cmd("enter-cpu-step")
        z.cmd("write-memory 40000 243 219 15 50 80 156 62 85 211 14 24 254")
        z.cmd("write-memory 40016 0")
        z.cmd("set-register PC=9C40H")
        for _ in range(6):
            z.cmd("cpu-step")
        got = z.cmd("read-memory 40016 1").split()[-1]
        state = z.cmd("get-registers") + "\n9C40: " + z.cmd("read-memory 40000 12")
        z.cmd("exit-cpu-step")
    finally:
        # A kill, not exit-emulator: ZEsarUX's exit saves a snapshot
        # (zesarux_autosave.zsf, beside the binary) that the next run
        # would start from, stopped in this test's JR $.
        p.kill()
        p.wait(10)

    checks = [
        (frames[:1] == [(4, 1)], "HELLO first, version 1"),
        ((2, 0) in frames, "IN A,(0Fh) arrived as op 2"),
        ((0, 0x55) in frames, "OUT (0Eh),A arrived as op 0, value 0x55"),
        (got.upper() == "%02X" % STATUS, "the bridge's 0x%02X reached the Z80 (read back %s)" % (STATUS, got)),
    ]
    for ok, msg in checks:
        print(("  PASS  " if ok else "  FAIL  ") + msg)
    print("frames:", frames[:12])
    if not all(ok for ok, _ in checks):
        print("after the steps:", state)
        sys.exit(1)
    print("ALL PASS")


if __name__ == "__main__":
    main()
