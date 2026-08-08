#!/usr/bin/env python3
"""Generate the project's Apache-2.0 KLV and MPEG-TS test corpus.

All values are invented and deterministic.  The script intentionally uses only
the Python standard library so core-only builds can generate their fixtures.
"""

# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import struct
import tomllib
from pathlib import Path

UAS_KEY = bytes.fromhex("060e2b34020b01010e01030101000000")


def ber_length(n: int) -> bytes:
    if n < 0x80:
        return bytes([n])
    raw = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(raw)]) + raw


def ber_oid(n: int) -> bytes:
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def tlv(tag: int, value: bytes) -> bytes:
    return ber_oid(tag) + ber_length(len(value)) + value


def bcc16(data: bytes) -> int:
    total = 0
    for i, byte in enumerate(data):
        total = (total + (byte << (8 * ((i + 1) % 2)))) & 0xFFFF
    return total


def packet(items: list[tuple[int, bytes]]) -> bytes:
    body = b"".join(tlv(tag, value) for tag, value in items if tag != 1)
    prefix = UAS_KEY + ber_length(len(body) + 4) + body + b"\x01\x02"
    return prefix + bcc16(prefix).to_bytes(2, "big")


def linear_raw(value: float, minimum: float, maximum: float, width: int,
               signed: bool) -> bytes:
    if signed:
        encoded = round(value * ((1 << (8 * width - 1)) - 1) / maximum)
        if encoded < 0:
            encoded += 1 << (8 * width)
    else:
        encoded = round((value - minimum) * ((1 << (8 * width)) - 1) /
                        (maximum - minimum))
    return encoded.to_bytes(width, "big")


def ordinary_value(item: dict) -> bytes:
    tag = item["tag"]
    kind = item["kind"]
    width = item.get("length", 0)
    if tag == 2:
        return (1_700_000_000_000_000).to_bytes(8, "big")
    if tag == 65:
        return b"\x13"  # ST 0601.19
    if kind == "utf8":
        return f"SYN-{tag}".encode()
    if kind in {"bytes", "nested_ls", "pack"}:
        return bytes([0xA0 | (tag & 0x0F), tag & 0xFF, 0x5A])
    if kind == "uint":
        return (tag & ((1 << (8 * width)) - 1)).to_bytes(width, "big")
    if kind == "int":
        return (-min(tag, (1 << (8 * width - 1)) - 1)).to_bytes(
            width, "big", signed=True)
    # Use a non-special, non-endpoint bit pattern. Decode/re-encode must retain it.
    return bytes([0x21]) + bytes((tag + i) & 0x7F for i in range(1, width))


def basic_stream() -> bytes:
    packets = []
    for i in range(6):
        timestamp = 1_700_000_000_000_000 + i * 100_000
        latitude = 12.5 + i * 0.01
        longitude = -45.25 + i * 0.01
        packets.append(packet([
            (2, timestamp.to_bytes(8, "big")),
            (3, b"SYNTHETIC-MISSION"),
            (4, b"SYNTHETIC-TAIL"),
            (10, b"TEST-PLATFORM"),
            (13, linear_raw(latitude, -90.0, 90.0, 4, True)),
            (14, linear_raw(longitude, -180.0, 180.0, 4, True)),
            (15, linear_raw(1250.0, -900.0, 19000.0, 2, False)),
            (23, linear_raw(latitude - 0.1, -90.0, 90.0, 4, True)),
            (24, linear_raw(longitude + 0.1, -180.0, 180.0, 4, True)),
            (47, b"\x05"),
            (59, b"SYNTH-01"),
            (65, b"\x13"),
            (72, timestamp.to_bytes(8, "big")),
        ]))
    return b"".join(packets)


def comprehensive_stream(registry: dict) -> bytes:
    items = registry["item"]
    ordinary = [(item["tag"], ordinary_value(item)) for item in items
                if item["tag"] != 1]

    specials = [(2, (1_700_000_000_500_000).to_bytes(8, "big"))]
    for item in items:
        if item.get("special"):
            width = item["length"]
            pattern = item["special"][0]["pattern"]
            if isinstance(pattern, str):
                pattern = int(pattern, 0)
            specials.append((item["tag"], pattern.to_bytes(width, "big")))

    variable_width = [(2, (1_700_000_001_000_000).to_bytes(8, "big"))]
    for item in items:
        if item.get("variable") and item["kind"] in {"uint", "int", "imapb"}:
            width = max(1, item["length"] - 1)
            variable_width.append((item["tag"], bytes([0x11]) * width))
    # Multi-byte BER-OID and unregistered raw passthrough are deliberate.
    variable_width.append((150, b"project-owned-opaque-value"))
    return packet(ordinary) + packet(specials) + packet(variable_width)


def mpeg_crc32(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 \
                else (crc << 1) & 0xFFFFFFFF
    return crc


def psi_packet(pid: int, section: bytes, continuity: int = 0) -> bytes:
    payload = b"\x00" + section
    return (bytes([0x47, 0x40 | (pid >> 8), pid & 0xFF, 0x10 | continuity]) +
            payload + bytes([0xFF]) * (184 - len(payload)))


def pat(pmt_pid: int) -> bytes:
    section = bytes.fromhex("00b00d0001c100000001") + struct.pack(">H", 0xE000 | pmt_pid)
    return section + mpeg_crc32(section).to_bytes(4, "big")


def pmt(streams: list[tuple[int, int]]) -> bytes:
    """Build a one-program PMT for project-authored elementary streams."""
    es = bytearray()
    for stream_type, es_pid in streams:
        descriptor = b"\x05\x04KLVA" if stream_type == 0x06 else b""
        es += bytes([stream_type, 0xE0 | (es_pid >> 8), es_pid & 0xFF,
                     0xF0, len(descriptor)]) + descriptor
    # The PCR PID only needs to name one elementary stream for these fixtures.
    pcr_pid = streams[0][1]
    section_length = 9 + len(es) + 4
    section = (bytes([0x02, 0xB0 | (section_length >> 8), section_length & 0xFF]) +
               bytes.fromhex("0001c10000") +
               struct.pack(">H", 0xE000 | pcr_pid) + b"\xF0\x00" + es)
    return section + mpeg_crc32(section).to_bytes(4, "big")


def encode_pts(pts: int) -> bytes:
    return bytes([
        0x21 | (((pts >> 30) & 0x07) << 1),
        (pts >> 22) & 0xFF,
        0x01 | (((pts >> 15) & 0x7F) << 1),
        (pts >> 7) & 0xFF,
        0x01 | ((pts & 0x7F) << 1),
    ])


def pes(payload: bytes, stream_type: int, pts: int | None,
        metadata_sequence: int = 0) -> bytes:
    stream_id = 0xFC if stream_type == 0x15 else (0xE0 if stream_type == 0x1B else 0xBD)
    if stream_type == 0x15:
        # ISO/IEC 13818-1 Metadata_AU_cell: service id 0, an incrementing
        # sequence number, and flags for one complete random-access cell
        # (fragmentation_indication=11, reserved bits=1111).  The extractor
        # only needs its 16-bit length today, but the fixture stays conformant.
        payload = (bytes([0x00, metadata_sequence & 0xFF, 0xDF]) +
                   len(payload).to_bytes(2, "big") + payload)
    optional = b"\x80\x80\x05" + encode_pts(pts) if pts is not None else b"\x80\x00\x00"
    length = len(optional) + len(payload)
    return b"\x00\x00\x01" + bytes([stream_id]) + length.to_bytes(2, "big") + optional + payload


def encode_pcr(pcr_90k: int) -> bytes:
    """Encode a 27 MHz PCR with a zero extension from a 90 kHz clock value."""
    base = pcr_90k & ((1 << 33) - 1)
    return bytes([(base >> 25) & 0xFF, (base >> 17) & 0xFF,
                  (base >> 9) & 0xFF, (base >> 1) & 0xFF,
                  ((base & 1) << 7) | 0x7E, 0x00])


def packetize_pes(data: bytes, pid: int, continuity: int,
                  pcr_90k: int | None = None) -> tuple[bytes, int]:
    out = bytearray()
    first = True
    while data:
        # A PCR occupies seven adaptation-field bytes (flags + six-byte PCR),
        # so reserve them on the first transport packet that carries one.
        take = min(176 if first and pcr_90k is not None else 184, len(data))
        chunk, data = data[:take], data[take:]
        pusi = 0x40 if first else 0
        if len(chunk) == 184:
            header = bytes([0x47, pusi | (pid >> 8), pid & 0xFF, 0x10 | continuity])
            out += header + chunk
        else:
            adaptation_length = 183 - len(chunk)
            header = bytes([0x47, pusi | (pid >> 8), pid & 0xFF, 0x30 | continuity])
            adaptation = bytes([adaptation_length])
            if adaptation_length:
                if first and pcr_90k is not None:
                    adaptation += b"\x10" + encode_pcr(pcr_90k)
                    adaptation += bytes([0xFF]) * (adaptation_length - 7)
                else:
                    adaptation += b"\x00" + bytes([0xFF]) * (adaptation_length - 1)
            out += header + adaptation + chunk
        continuity = (continuity + 1) & 0x0F
        first = False
    return bytes(out), continuity


def split_packets(stream: bytes) -> list[bytes]:
    packets = []
    offset = 0
    while offset < len(stream):
        if stream[offset:offset + 16] != UAS_KEY:
            raise ValueError(f"KLV framing lost at offset {offset}")
        first_len = stream[offset + 16]
        if first_len < 0x80:
            value_len, length_len = first_len, 1
        else:
            count = first_len & 0x7F
            value_len = int.from_bytes(stream[offset + 17:offset + 17 + count], "big")
            length_len = 1 + count
        size = 16 + length_len + value_len
        packets.append(stream[offset:offset + size])
        offset += size
    return packets


def transport_stream(klv: bytes, stream_type: int, timed: bool) -> bytes:
    pmt_pid, es_pid = 0x100, 0x101
    out = bytearray(psi_packet(0, pat(pmt_pid)) +
                    psi_packet(pmt_pid, pmt([(stream_type, es_pid)])))
    continuity = 0
    for i, klv_packet in enumerate(split_packets(klv)):
        timestamp = 90_000 + i * 9_000 if timed else None
        # The PMT names this ES as its PCR PID. PCR is present even for the
        # deliberately untimed PES variant; PCR and PTS are separate clocks.
        ts_packets, continuity = packetize_pes(
            pes(klv_packet, stream_type, timestamp, i), es_pid, continuity,
            90_000 + i * 9_000)
        out += ts_packets
    return bytes(out)


# A tiny authored H.264 access unit: AUD + baseline-profile SPS/PPS + an IDR
# slice.  This is deliberately data, not an encoded image fixture: the project
# needs only a valid H.264 carrier to exercise the no-decode passthrough path.
# Repeating the self-contained unit gives every PES a random-access frame and
# keeps the carrier deterministic without invoking an encoder at build time.
H264_ACCESS_UNIT = bytes.fromhex(
    "0000000109f0"
    "000000016742c01eda01e0089f97011000000300100000030028f1831960"
    "0000000168ce3c80"
    "000000016588843a08400227e5c044000003000400000300ca3c489e"
)


def video_transport_stream(source_klv: bytes) -> bytes:
    """A short H.264+KLV TS that proves insertion drops source-side metadata."""
    pmt_pid, klv_pid, video_pid = 0x100, 0x101, 0x102
    out = bytearray(psi_packet(0, pat(pmt_pid)) +
                    psi_packet(pmt_pid, pmt([(0x1B, video_pid), (0x06, klv_pid)])))
    video_continuity = 0
    klv_continuity = 0
    # This deliberately differs from synthetic-basic.klv. The integration test
    # verifies it is not forwarded beside the KLV provided to open_insert().
    packets, klv_continuity = packetize_pes(
        pes(source_klv, 0x06, 0), klv_pid, klv_continuity)
    out += packets
    # Two seconds at 30 fps leaves ample video for the six 100-ms KLV packets.
    for frame in range(60):
        packets, video_continuity = packetize_pes(
            pes(H264_ACCESS_UNIT, 0x1B, frame * 3_000), video_pid, video_continuity,
            frame * 3_000)
        out += packets
    return bytes(out)


def validate_transport_fixture(ts: bytes, pcr_pid: int,
                               metadata_au: bool = False) -> None:
    """Fail generation if a compact TS fixture loses its required wire shape."""
    if len(ts) % 188:
        raise ValueError("transport stream is not an integral number of packets")
    pcrs = 0
    sequences: list[int] = []
    for offset in range(0, len(ts), 188):
        packet = ts[offset:offset + 188]
        if packet[0] != 0x47:
            raise ValueError("transport packet lacks sync byte")
        pid = ((packet[1] & 0x1F) << 8) | packet[2]
        afc = (packet[3] >> 4) & 0x03
        payload = 4
        if afc & 0x02:
            adaptation_length = packet[payload]
            if payload + 1 + adaptation_length > 188:
                raise ValueError("transport adaptation field overruns packet")
            if adaptation_length >= 7 and packet[payload + 1] & 0x10 and pid == pcr_pid:
                pcrs += 1
            payload += 1 + adaptation_length
        if (metadata_au and pid == 0x101 and (afc & 0x01) and
                packet[1] & 0x40 and packet[payload:payload + 4] == b"\x00\x00\x01\xFC"):
            header = payload + 9 + packet[payload + 8]
            cell = packet[header:header + 5]
            if len(cell) != 5 or cell[0] != 0 or cell[2] != 0xDF:
                raise ValueError("invalid RP 217 metadata AU cell header")
            cell_length = int.from_bytes(cell[3:5], "big")
            pes_length = int.from_bytes(packet[payload + 4:payload + 6], "big")
            if cell_length != pes_length - (3 + packet[payload + 8]) - 5:
                raise ValueError("RP 217 metadata AU cell length disagrees with PES")
            sequences.append(cell[1])
    if not pcrs:
        raise ValueError(f"PMT PCR PID 0x{pcr_pid:04x} carries no PCR")
    if metadata_au and sequences != list(range(len(sequences))):
        raise ValueError("RP 217 metadata AU sequence numbers do not increment")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("registry", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    registry = tomllib.loads(args.registry.read_text())
    args.output.mkdir(parents=True, exist_ok=True)

    basic = basic_stream()
    comprehensive = comprehensive_stream(registry)
    source_klv = packet([
        (2, (1_700_000_123_456_000).to_bytes(8, "big")),
        (3, b"SOURCE-SIDE-METADATA"),
        (10, b"SOURCE-ONLY-PLATFORM"),
    ])
    timed_06 = transport_stream(basic, 0x06, True)
    untimed_06 = transport_stream(basic, 0x06, False)
    timed_15 = transport_stream(basic, 0x15, True)
    video = video_transport_stream(source_klv)
    validate_transport_fixture(timed_06, 0x101)
    validate_transport_fixture(untimed_06, 0x101)
    validate_transport_fixture(timed_15, 0x101, metadata_au=True)
    validate_transport_fixture(video, 0x102)
    outputs = {
        "synthetic-basic.klv": basic,
        "synthetic-first-packet.klv": split_packets(basic)[0],
        "synthetic-comprehensive.klv": comprehensive,
        "synthetic-timed-0x06.ts": timed_06,
        "synthetic-untimed-0x06.ts": untimed_06,
        "synthetic-timed-0x15.ts": timed_15,
        "synthetic-video.ts": video,
    }
    for name, content in outputs.items():
        (args.output / name).write_bytes(content)
        print(f"wrote {args.output / name} ({len(content)} bytes)")


if __name__ == "__main__":
    main()
