#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Author a hand-built ST 0601 packet nesting an ST 0903 VMTI LS in Item 74.

No public byte-level VMTI example exists in the standards, so this constructs a
spec-conformant fixture *independently of the C++ encoder* — the round-trip test
then validates recursive parse+build against these authored bytes (not against
its own output). Writes test/fixtures/vmti_nested.klv.
"""
import os
import sys

UAS_KEY = bytes.fromhex("060e2b34020b01010e01030101000000")  # 16 bytes
TS = bytes.fromhex("00046c8e20038385")  # 8-byte Precision Time Stamp (µs)


def ber_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def tlv(tag, value):  # 1-byte BER-OID tag (all tags here < 128)
    return bytes([tag]) + ber_len(len(value)) + value


def bcc16(data):  # ST 0601 §6.6
    s = 0
    for i, byte in enumerate(data):
        s = (s + (byte << (8 * ((i + 1) % 2)))) & 0xFFFF
    return s


# --- nested VMTI LS (ST 0903): value of Item 74, no key/length/checksum ------
vmti = b"".join([
    tlv(2, TS),          # 02 Precision Time Stamp (uint64)
    tlv(4, bytes([6])),  # 04 VMTI LS Version Number = 6
    tlv(5, bytes([3])),  # 05 Total Targets Detected = 3
    tlv(6, bytes([2])),  # 06 Number Reported = 2
])

# --- outer ST 0601 packet ---------------------------------------------------
items = b"".join([
    tlv(2, TS),          # Item 2 Precision Time Stamp
    tlv(74, vmti),       # Item 74 VMTI Local Set (nested)
])
value_len = len(items) + 4               # + checksum item (01 02 XX XX)
packet = UAS_KEY + ber_len(value_len) + items + bytes([0x01, 0x02])
cs = bcc16(packet)
packet += bytes([(cs >> 8) & 0xFF, cs & 0xFF])

out = os.path.join(os.path.dirname(__file__), "vmti_nested.klv")
with open(out, "wb") as f:
    f.write(packet)
print(f"wrote {out} ({len(packet)} bytes); VMTI value {len(vmti)} B; checksum 0x{cs:04X}")
