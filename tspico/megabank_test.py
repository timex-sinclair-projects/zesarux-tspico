#!/usr/bin/env python3
"""The TS-2068 Megabank model (MEGABANK.md) in ZEsarUX: stock ROM, no other
files needed, so CI can run it anywhere.

    python3 tspico/megabank_test.py src/zesarux

ZEsarUX runs a TS2068 headless with its remote protocol (ZRCP) on. With the
CPU stopped, the test writes the board's registers through write-port, reads
and writes memory through the paging, steps a short routine that reads the
registers with IN, resets, and power-cycles ZEsarUX to check the battery file.
"""
import os, socket, subprocess, sys, tempfile, time

ZX = os.path.abspath(sys.argv[1])
PORT = 10123
WORK = tempfile.mkdtemp(prefix="megabank-")
FILE = os.path.join(WORK, "megabank.bin")
fails = 0


def check(name, got, want):
    global fails
    ok = got == want
    fails += not ok
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": got {got!r}, want {want!r}"))


class Z:
    def __init__(self):
        for _ in range(50):
            try:
                self.s = socket.create_connection(("127.0.0.1", PORT), timeout=2)
                break
            except OSError:
                time.sleep(0.2)
        self.read_prompt()

    def read_prompt(self):
        d = b""
        while not (d.endswith(b"command> ") or d.endswith(b"cpu-step> ")):
            d += self.s.recv(65536)
        return d.decode(errors="replace")

    def cmd(self, c):
        self.s.sendall((c + "\n").encode())
        if c == "exit-emulator":
            return ""
        out = self.read_prompt()
        return out.rsplit("\n", 1)[0].strip()

    def port(self, p, v):
        self.cmd(f"write-port {p} {v}")

    def reg(self, r, v):          # select register r on FDh, write v on FCh
        self.port(0xFD, r)
        self.port(0xFC, v)

    def peek(self, addr, n=1):
        return self.cmd(f"read-memory {addr} {n}").upper()

    def poke(self, addr, *vals):
        self.cmd("write-memory %d %s" % (addr, " ".join(str(v) for v in vals)))


def launch(env_extra):
    env = dict(os.environ, **env_extra)
    return subprocess.Popen([ZX, "--machine", "TS2068", "--noconfigfile",
                             "--vo", "null", "--ao", "null", "--enable-remoteprotocol",
                             "--remoteprotocol-port", str(PORT)],
                            cwd=os.path.dirname(ZX), env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# IN test routine, at A000h (chunk 5): reads ID, IDCOMP, OWN4, IN FDh, BASE, SIZE, an unused register
PROG = [0xF3]
for i, r in enumerate([0x12, 0x13, 0x04, None, 0x14, 0x15, 0x77]):
    if r is None:
        PROG += [0xDB, 0xFD]                       # IN A,(FDh): the selected register
    else:
        PROG += [0x3E, r, 0xD3, 0xFD, 0xDB, 0xFC]  # LD A,r / OUT (FDh),A / IN A,(FCh)
    PROG += [0x32, i, 0xA1]                        # LD (A100h+i),A
PROG += [0x18, 0xFE]                               # JR $


def run_prog(z):
    z.poke(0xA000, *PROG)
    z.cmd("set-register PC=40960")
    for _ in range(40):
        z.cmd("cpu-step")
    return z.peek(0xA100, 7)


if os.path.exists(FILE):
    os.remove(FILE)
p = launch({"MEGABANK_FILE": FILE})
time.sleep(3)
z = Z()
z.cmd("enter-cpu-step")

page_home = z.peek(0x8000, 4)
z.reg(0x04, 5)                                       # OWN4 = bank 5 (SUB4 = 4)
z.poke(0x8000, 0xDE, 0xAD, 0xBE, 0xEF)
check("bank 5 page 4 in chunk 4 holds a write", z.peek(0x8000, 4), "DEADBEEF")
z.reg(0x0C, 2)                                       # SUB4 = 2
check("SUB4 = 2 shows another page", z.peek(0x8000, 4), "00000000")
z.reg(0x0C, 4)
check("SUB4 = 4 again", z.peek(0x8000, 4), "DEADBEEF")
z.reg(0x04, 0)
check("OWN4 = 0 gives HOME back", z.peek(0x8000, 4), page_home)
z.reg(0x04, 200)
check("a bank not on the board leaves HOME", z.peek(0x8000, 4), page_home)
z.reg(0x0C, 2)
z.reg(0x04, 5)
check("writing OWN4 resets SUB4 to 4", z.peek(0x8000, 4), "DEADBEEF")

z.reg(0x10, 0x10)                                    # WPROT bit 4
z.poke(0x8000, 0x11)
check("WPROT drops the write", z.peek(0x8000, 1), "DE")
z.reg(0x10, 0)
z.poke(0x8000, 0xDE)

rom = z.peek(0x2000, 2)
z.reg(0x01, 5)                                       # RAM over the ROM, chunk 1
z.poke(0x2000, 0x12, 0x34)
check("chunk 1 is board RAM", z.peek(0x2000, 2), "1234")
z.reg(0x01, 0)
check("chunk 1 is ROM again", z.peek(0x2000, 2), rom)

z.reg(0x04, 6)                                       # bank 6 page 4 in chunk 4, page 5 in chunk 5
z.poke(0x8000, 0x66)
z.reg(0x04, 5)
check("banks are separate pages", z.peek(0x8000, 1), "DE")

check("IN: ID, IDCOMP, OWN4, IN FDh, BASE, SIZE, unused", run_prog(z), "A55A050401" + "40" + "FF")

z.cmd("reset-cpu")
time.sleep(3)
z.cmd("enter-cpu-step")
check("reset: OWN4 cleared (IN reads 0)", run_prog(z)[4:6], "00")

z.cmd("exit-emulator")
p.wait(timeout=10)
data = open(FILE, "rb").read()
off = ((5 - 1) * 8 + 4) * 8192
check("battery file is 4 MB", len(data), 4 * 1024 * 1024)
check("battery file holds bank 5 page 4", data[off:off + 4].hex().upper(), "DEADBEEF")

p = launch({"MEGABANK_FILE": FILE, "MEGABANK_WP": "1"})
time.sleep(3)
z = Z()
z.cmd("enter-cpu-step")
z.reg(0x04, 5)
check("after a power cycle the data is there", z.peek(0x8000, 4), "DEADBEEF")
z.poke(0x8000, 0)
check("write-protect switch drops writes", z.peek(0x8000, 1), "DE")
z.cmd("exit-emulator")
p.wait(timeout=10)

p = launch({"MEGABANK": "1", "MEGABANK_BASE": "193"})
time.sleep(3)
z = Z()
z.cmd("enter-cpu-step")
home = z.peek(0x8000, 4)
z.reg(0x04, 253)
z.poke(0x8000, 0x25, 0x30)
check("BASE 193: bank 253 is on the board", z.peek(0x8000, 2), "2530")
z.reg(0x04, 254)
check("BASE 193: owner FEh is none", z.peek(0x8000, 4), home)
check("BASE 193: IN reads BASE C1h and SIZE 40h", run_prog(z)[8:12], "C140")
z.cmd("exit-emulator")
p.wait(timeout=10)

p = launch({})
time.sleep(3)
z = Z()
z.cmd("enter-cpu-step")
home = z.peek(0x8000, 4)
z.reg(0x04, 5)
check("no board without MEGABANK/MEGABANK_FILE", z.peek(0x8000, 4), home)
z.cmd("exit-emulator")
p.wait(timeout=10)

print("PASS" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
