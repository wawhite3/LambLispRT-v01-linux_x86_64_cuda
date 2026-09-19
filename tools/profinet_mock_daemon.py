#!/usr/bin/env python3
# Copyright 2026 by Frobenius Norm LLC 2026-08-20 23:30:00
# Free for non-commercial use. Commercial use requires a license.
#
# profinet_mock_daemon.py -- a MOCK of the P145 PROFINET daemon.
#
# Speaks the ll_vm_profinet_ipc.h protocol so the whole LambLisp side --
# handle table, device model, process image, event queue, error mapping -- is
# testable with NO p-net, no GPLv3 code, no raw sockets, no controller and no
# network.  It is the PROFINET analogue of the PROFIBUS pty loopback.
#
# It is NOT a PROFINET stack.  It fakes the LIFECYCLE (offline -> dcp-only ->
# connected -> operate), loops provider data back as consumer data so a round
# trip is observable, and can inject DCP events on cue.  Anything about actual
# wire behaviour must be tested against the real daemon and a real controller.
#
# Usage:
#   python3 w3_ai_scripts/profinet_mock_daemon.py [--path SOCK] [--script FILE]
#
# --script feeds scripted events, one per line, consumed by POLL_EVENT:
#   set-name lamb-io-1
#   set-ip 192.168.0.50 255.255.255.0 192.168.0.1
#   connect | param-end | app-ready | release | signal
#
import argparse
import os
import socket
import struct
import sys

HDR = 12
MAGIC0, MAGIC1, VERSION = ord('P'), ord('N'), 1

CMD = dict(HELLO=0x01, ADD_MODULE=0x02, ADD_SUBMODULE=0x03, START=0x04, STOP=0x05,
           SET_INPUTS=0x06, GET_OUTPUTS=0x07, POLL_EVENT=0x08, STATE=0x09,
           STATS=0x0A, ALARM=0x0B, SET_IOPS=0x0C)
ST = dict(OK=0, EMPTY=1, BADCMD=2, BADARG=3, NOTREADY=4, NOSUB=5, INTERNAL=6)
EV = dict(SET_NAME=0x01, SET_IP=0x02, CONNECT=0x03, PARAM_END=0x04,
          APP_READY=0x05, RELEASE=0x06, ALARM=0x07, SIGNAL=0x08)
OFFLINE, DCP_ONLY, CONNECTED, OPERATE = 0, 1, 2, 3
IOXS_GOOD = 0x80


def hdr_put(cmd, status, flags, slot, subslot, length):
    return struct.pack("!BBBBBBHHH", MAGIC0, MAGIC1, VERSION, cmd, status, flags,
                       slot, subslot, length)


def hdr_get(b):
    m0, m1, ver, cmd, st, fl, slot, sub, ln = struct.unpack("!BBBBBBHHH", b)
    if m0 != MAGIC0 or m1 != MAGIC1 or ver != VERSION:
        raise ValueError("bad IPC header magic/version")
    return cmd, st, fl, slot, sub, ln


class MockDevice:
    def __init__(self, script):
        self.submods = {}          # (slot, subslot) -> dict
        self.state = OFFLINE
        self.events = []
        self.station = b""
        self.cycles = 0
        self.alarms = 0
        self.load_script(script)

    def load_script(self, path):
        if not path or not os.path.exists(path):
            return
        for raw in open(path):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            k = parts[0]
            if k == "set-name":
                self.events.append(bytes([EV["SET_NAME"]]) + parts[1].encode())
            elif k == "set-ip":
                payload = b"".join(bytes(int(o) for o in p.split(".")) for p in parts[1:4])
                self.events.append(bytes([EV["SET_IP"]]) + payload)
            elif k in ("connect", "param-end", "app-ready", "release", "signal"):
                key = {"connect": "CONNECT", "param-end": "PARAM_END",
                       "app-ready": "APP_READY", "release": "RELEASE",
                       "signal": "SIGNAL"}[k]
                self.events.append(bytes([EV[key]]))

    def handle(self, cmd, flags, slot, sub, payload):
        """Return (status, flags, payload)."""
        if cmd == CMD["HELLO"]:
            return ST["OK"], 0, b"mock-profinet-daemon/1"

        if cmd == CMD["ADD_MODULE"]:
            return (ST["OK"], 0, b"") if len(payload) == 4 else (ST["BADARG"], 0, b"")

        if cmd == CMD["ADD_SUBMODULE"]:
            if len(payload) != 8:
                return ST["BADARG"], 0, b""
            _ident, in_len, out_len = struct.unpack("!IHH", payload)
            self.submods[(slot, sub)] = dict(in_len=in_len, out_len=out_len,
                                             data=bytearray(in_len), iops=IOXS_GOOD)
            return ST["OK"], 0, b""

        if cmd == CMD["START"]:
            self.station = payload
            # A real device answers DCP before a controller connects; the mock
            # jumps straight to OPERATE so tests need no controller.
            self.state = OPERATE
            return ST["OK"], 0, b""

        if cmd == CMD["STOP"]:
            self.state = OFFLINE
            return ST["OK"], 0, b""

        if cmd == CMD["SET_INPUTS"]:
            s = self.submods.get((slot, sub))
            if s is None:
                return ST["NOSUB"], 0, b""
            s["data"] = bytearray(payload)
            s["iops"] = flags
            self.cycles += 1
            return ST["OK"], 0, b""

        if cmd == CMD["GET_OUTPUTS"]:
            s = self.submods.get((slot, sub))
            if s is None:
                return ST["NOSUB"], 0, b""
            if self.state != OPERATE:
                return ST["NOTREADY"], 0, b""
            # Loop provider data back as consumer data, truncated/padded to
            # out_len, so a Scheme test can observe a full round trip.
            out = bytes(s["data"][:s["out_len"]]).ljust(s["out_len"], b"\x00")
            return ST["OK"], IOXS_GOOD, out

        if cmd == CMD["SET_IOPS"]:
            s = self.submods.get((slot, sub))
            if s is None:
                return ST["NOSUB"], 0, b""
            s["iops"] = flags
            return ST["OK"], 0, b""

        if cmd == CMD["POLL_EVENT"]:
            if not self.events:
                return ST["EMPTY"], 0, b""
            return ST["OK"], 0, self.events.pop(0)

        if cmd == CMD["STATE"]:
            return ST["OK"], 0, bytes([self.state])

        if cmd == CMD["STATS"]:
            vals = [self.cycles, 0, self.cycles & 0xFFFF, 0, self.alarms, 0]
            return ST["OK"], 0, b"".join(struct.pack("!I", v) for v in vals)

        if cmd == CMD["ALARM"]:
            self.alarms += 1
            return ST["OK"], 0, b""

        return ST["BADCMD"], 0, b""


def serve(conn, dev):
    while True:
        head = b""
        while len(head) < HDR:
            chunk = conn.recv(HDR - len(head))
            if not chunk:
                return
            head += chunk
        cmd, _st, flags, slot, sub, ln = hdr_get(head)
        body = b""
        while len(body) < ln:
            chunk = conn.recv(ln - len(body))
            if not chunk:
                return
            body += chunk
        status, rflags, payload = dev.handle(cmd, flags, slot, sub, body)
        conn.sendall(hdr_put(cmd, status, rflags, slot, sub, len(payload)) + payload)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--path", default="/tmp/lamblisp-profinet.sock")
    ap.add_argument("--script", default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if os.path.exists(args.path):
        os.unlink(args.path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(args.path)
    srv.listen(2)
    print(args.path, flush=True)
    if not args.quiet:
        print("mock-profinet: listening on %s" % args.path, file=sys.stderr, flush=True)
    try:
        while True:
            conn, _ = srv.accept()
            try:
                serve(conn, MockDevice(args.script))
            except (ValueError, ConnectionError, OSError) as e:
                if not args.quiet:
                    print("mock-profinet: %s" % e, file=sys.stderr, flush=True)
            finally:
                conn.close()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()
        if os.path.exists(args.path):
            os.unlink(args.path)


if __name__ == "__main__":
    main()
