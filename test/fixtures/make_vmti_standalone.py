#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Author a standalone ST 0903 VMTI LS packet (its own UL key + checksum).

Standalone-VMTI = a top-level KLV packet on its own PID, distinct from the
embedded (0601 Item 74) form. Per ST 0903 §10.1.1 / req 0903.6-119 the checksum
is the ST 0601 algorithm over the whole LS (16-byte key .. checksum length).
Writes test/fixtures/vmti_standalone.klv.
"""
import os

# ST 0903 VMTI LS UL key (ST 0903 §10.1, CRC 51307)
VMTI_KEY = bytes.fromhex("060e2b34020b01010e01030306000000")  # 16 bytes
TS = bytes.fromhex("00046c8e20038385")  # 8-byte Precision Time Stamp (µs)


def ber_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def tlv(tag, value):
    return bytes([tag]) + ber_len(len(value)) + value


def bcc16(data):  # ST 0601 §6.6, reused by ST 0903 standalone (0903.6-119)
    s = 0
    for i, byte in enumerate(data):
        s = (s + (byte << (8 * ((i + 1) % 2)))) & 0xFFFF
    return s


items = b"".join([
    tlv(2, TS),                       # 02 Precision Time Stamp
    tlv(4, bytes([6])),               # 04 VMTI LS Version Number = 6
    tlv(5, bytes([3])),               # 05 Total Targets Detected = 3
    tlv(6, bytes([2])),               # 06 Number Reported = 2
    tlv(11, bytes.fromhex("0640")),   # 11 Horizontal FoV = IMAPB(0,180) 12.5°
    tlv(12, bytes.fromhex("0500")),   # 12 Vertical FoV   = IMAPB(0,180) 10.0°
])
value_len = len(items) + 4            # + checksum item (01 02 XX XX), last
packet = VMTI_KEY + ber_len(value_len) + items + bytes([0x01, 0x02])
cs = bcc16(packet)
packet += bytes([(cs >> 8) & 0xFF, cs & 0xFF])

out = os.path.join(os.path.dirname(__file__), "vmti_standalone.klv")
with open(out, "wb") as f:
    f.write(packet)
print(f"wrote {out} ({len(packet)} bytes); checksum 0x{cs:04X}")
