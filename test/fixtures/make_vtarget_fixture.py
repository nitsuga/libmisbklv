#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Author a standalone VMTI packet carrying a vTargetSeries (ST 0903 Item 101).

Mirrors ST 0903 Figure 13: Item 101 (L=30) = two VTarget Packs (L=13, L=15),
each [BER-OID targetId][VTarget LS items]. IMAPB offsets use the standard's
worked values (10.0°->0x3A6667, 12.0°->0x3E6667). Writes vmti_vtarget.klv.
"""
import os

VMTI_KEY = bytes.fromhex("060e2b34020b01010e01030306000000")
TS = bytes.fromhex("00046c8e20038385")


def ber_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def tlv(tag, value):
    return bytes([tag]) + ber_len(len(value)) + value


def elem(body):                       # a Series element: length-prefixed
    return ber_len(len(body)) + body


def bcc16(data):
    s = 0
    for i, byte in enumerate(data):
        s = (s + (byte << (8 * ((i + 1) % 2)))) & 0xFFFF
    return s


# VTarget Pack 1 (targetId=1): centroid, color, priority — all uint
pack1 = bytes([0x01]) + b"".join([          # BER-OID targetId = 1
    tlv(1, bytes.fromhex("0123")),          # 01 targetCentroid
    tlv(8, bytes.fromhex("012334")),        # 08 targetColor (uint24)
    tlv(4, bytes([0x07])),                  # 04 targetPriority
])
# VTarget Pack 2 (targetId=2): centroid + IMAPB lat/lon offsets
pack2 = bytes([0x02]) + b"".join([          # BER-OID targetId = 2
    tlv(1, bytes.fromhex("0246")),          # 01 targetCentroid
    tlv(10, bytes.fromhex("3A6667")),       # 10 offsetLat  = 10.0° IMAPB(-19.2,19.2,3)
    tlv(11, bytes.fromhex("3E6667")),       # 11 offsetLon  = 12.0°
])
assert len(pack1) == 13 and len(pack2) == 15, (len(pack1), len(pack2))
vtarget_series = elem(pack1) + elem(pack2)  # Item 101 value (L should be 30)
assert len(vtarget_series) == 30, len(vtarget_series)

items = b"".join([
    tlv(2, TS),                # 02 Precision Time Stamp
    tlv(4, bytes([6])),        # 04 VMTI LS Version Number
    tlv(5, bytes([3])),        # 05 Total Targets Detected
    tlv(6, bytes([2])),        # 06 Number Reported
    tlv(101, vtarget_series),  # 101 vTargetSeries
])
value_len = len(items) + 4
packet = VMTI_KEY + ber_len(value_len) + items + bytes([0x01, 0x02])
cs = bcc16(packet)
packet += bytes([(cs >> 8) & 0xFF, cs & 0xFF])

out = os.path.join(os.path.dirname(__file__), "vmti_vtarget.klv")
with open(out, "wb") as f:
    f.write(packet)
print(f"wrote {out} ({len(packet)} bytes); series {len(vtarget_series)} B; checksum 0x{cs:04X}")
