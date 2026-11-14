#!/usr/bin/env python3
# Copyright 2026 by Frobenius Norm LLC 2026-08-22
# Free for non-commercial use. Commercial use requires a license.
#
# profinet_dcp_probe.py -- send a PROFINET DCP Identify-All request and print
# the devices that answer.  This is what an engineering tool does first when it
# looks at a segment, so it is the smallest real proof that a PROFINET device
# is alive on the wire: no PLC, no controller, no licence.
#
# Constants verified against the DCP dissector/implementation references and
# the P145 proposal (which had them transposed in its first draft):
#   EtherType 0x8892
#   FrameID   0xFEFE Identify request   0xFEFF Identify response
#   multicast 01:0E:CF:00:00:00
#   ServiceID 5 (Identify) + ServiceType 0 (Request)
#
# Needs CAP_NET_RAW.  Usage:
#   sudo python3 w3_ai_scripts/profinet_dcp_probe.py --if pn1 [--timeout 3]
import argparse
import socket
import struct
import sys
import time

ETH_P_ALL = 0x0003
PN_ETHERTYPE = 0x8892
FID_IDENT_REQ = 0xFEFE
FID_IDENT_RES = 0xFEFF
FID_GETSET = 0xFEFD
MCAST = bytes.fromhex("010ecf000000")

SVC_SET, SVC_IDENTIFY, TYPE_REQUEST = 4, 5, 0
OPT_IP, SUB_IP_PARAM = 1, 2
OPT_DEVICE = 2
SUB_DEV_VENDOR, SUB_DEV_NAME, SUB_DEV_ID, SUB_DEV_ROLE = 1, 2, 3, 4
OPT_ALL, SUB_ALL = 0xFF, 0xFF


def build_identify_all(src_mac, xid=0x0F000001):
    """Ethernet + DCP Identify-All request."""
    eth = MCAST + src_mac + struct.pack("!H", PN_ETHERTYPE)
    # One block: option 0xFF/0xFF = "all", length 0 -> identify every device.
    blocks = struct.pack("!BBH", OPT_ALL, SUB_ALL, 0)
    dcp = struct.pack("!BBIHH", SVC_IDENTIFY, TYPE_REQUEST, xid, 1, len(blocks)) + blocks
    frame = eth + struct.pack("!H", FID_IDENT_REQ) + dcp
    return frame.ljust(60, b"\x00")


def build_set(src_mac, dst_mac, opt, sub, value, permanent=False, xid=0x0F000002):
    """DCP Set request.  Unicast to the device.

    A Set block carries a 2-byte BlockQualifier (not BlockInfo): bit 0 set
    means "permanent", i.e. survive a power cycle.  Blocks pad to even length.
    """
    qualifier = struct.pack("!H", 1 if permanent else 0)
    payload = qualifier + value
    block = struct.pack("!BBH", opt, sub, len(payload)) + payload
    if len(block) % 2:
        block += b"\x00"
    dcp = struct.pack("!BBIHH", SVC_SET, TYPE_REQUEST, xid, 0, len(block)) + block
    eth = dst_mac + src_mac + struct.pack("!H", PN_ETHERTYPE)
    return (eth + struct.pack("!H", FID_GETSET) + dcp).ljust(60, b"\x00")


def parse_blocks(data):
    """Walk the DCP blocks of a response.  Blocks pad to an even length."""
    out, off = [], 0
    while off + 4 <= len(data):
        opt, sub, blen = struct.unpack_from("!BBH", data, off)
        off += 4
        val = data[off:off + blen]
        off += blen + (blen % 2)          # pad to even
        out.append((opt, sub, val))
    return out


def describe(opt, sub, val):
    # Response blocks lead with a 2-byte BlockInfo before the value.
    body = val[2:] if len(val) >= 2 else b""
    if (opt, sub) == (OPT_DEVICE, SUB_DEV_NAME):
        return "NameOfStation", body.decode("ascii", "replace")
    if (opt, sub) == (OPT_DEVICE, SUB_DEV_VENDOR):
        return "DeviceVendor", body.decode("ascii", "replace")
    if (opt, sub) == (OPT_DEVICE, SUB_DEV_ID) and len(body) >= 4:
        v, d = struct.unpack("!HH", body[:4])
        return "DeviceID", "vendor 0x%04X device 0x%04X" % (v, d)
    if (opt, sub) == (OPT_DEVICE, SUB_DEV_ROLE) and body:
        return "DeviceRole", "0x%02X" % body[0]
    if (opt, sub) == (OPT_IP, SUB_IP_PARAM) and len(body) >= 12:
        ip, mask, gw = (socket.inet_ntoa(body[i:i + 4]) for i in (0, 4, 8))
        return "IPParameter", "ip %s mask %s gw %s" % (ip, mask, gw)
    return "option %d/%d" % (opt, sub), val.hex()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--if", dest="ifname", required=True)
    ap.add_argument("--timeout", type=float, default=3.0)
    ap.add_argument("--set-name", help="after discovery, DCP Set NameOfStation")
    ap.add_argument("--set-ip", help="after discovery, DCP Set IP as ip/mask/gw")
    ap.add_argument("--permanent", action="store_true",
                    help="ask the device to persist the setting")
    args = ap.parse_args()

    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    s.bind((args.ifname, 0))
    src_mac = s.getsockname()[4]
    s.settimeout(0.3)

    req = build_identify_all(src_mac)
    s.send(req)
    print("sent DCP Identify-All on %s (%d bytes, FrameID 0x%04X)"
          % (args.ifname, len(req), FID_IDENT_REQ))

    found, last_mac, deadline = 0, None, time.time() + args.timeout
    while time.time() < deadline:
        try:
            frame = s.recv(2048)
        except socket.timeout:
            continue
        if len(frame) < 16:
            continue
        ethertype = struct.unpack_from("!H", frame, 12)[0]
        off = 14
        if ethertype == 0x8100:                       # VLAN tagged
            ethertype = struct.unpack_from("!H", frame, 16)[0]
            off = 18
        if ethertype != PN_ETHERTYPE:
            continue
        frame_id = struct.unpack_from("!H", frame, off)[0]
        if frame_id != FID_IDENT_RES:
            continue

        src = ":".join("%02x" % b for b in frame[6:12])
        last_mac = frame[6:12]
        svc, styp, xid, _delay, dlen = struct.unpack_from("!BBIHH", frame, off + 2)
        print("\nDCP Identify RESPONSE from %s  (FrameID 0x%04X, xid 0x%08X)"
              % (src, frame_id, xid))
        for opt, sub, val in parse_blocks(frame[off + 12: off + 12 + dlen]):
            k, v = describe(opt, sub, val)
            print("   %-14s %s" % (k, v))
        found += 1

    print("\n%d device(s) answered" % found)
    if found == 0:
        return 1

    # ---- optional commissioning: what an engineering tool does next ----
    if args.set_name:
        value = args.set_name.encode("ascii")
        s.send(build_set(src_mac, last_mac, OPT_DEVICE, SUB_DEV_NAME, value, args.permanent))
        print("sent DCP Set NameOfStation = '%s'%s"
              % (args.set_name, " (permanent)" if args.permanent else ""))
    if args.set_ip:
        try:
            ip, mask, gw = args.set_ip.split("/")
        except ValueError:
            print("--set-ip wants ip/mask/gw", file=sys.stderr)
            return 2
        value = socket.inet_aton(ip) + socket.inet_aton(mask) + socket.inet_aton(gw)
        s.send(build_set(src_mac, last_mac, OPT_IP, SUB_IP_PARAM, value, args.permanent))
        print("sent DCP Set IP = %s / %s / %s" % (ip, mask, gw))

    if args.set_name or args.set_ip:
        # Drain any Set responses so a failure is visible rather than assumed.
        deadline = time.time() + 2.0
        while time.time() < deadline:
            try:
                frame = s.recv(2048)
            except socket.timeout:
                continue
            if len(frame) > 16 and struct.unpack_from("!H", frame, 12)[0] == PN_ETHERTYPE:
                fid = struct.unpack_from("!H", frame, 14)[0]
                if fid == FID_GETSET:
                    svc, styp = struct.unpack_from("!BB", frame, 16)
                    print("Set response: ServiceID %d ServiceType %d%s"
                          % (svc, styp, "  (OK)" if styp == 1 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
