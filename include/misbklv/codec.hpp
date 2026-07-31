// SPDX-License-Identifier: Apache-2.0
// Value codecs (raw-int / linear-LDS / IMAPB / utf8) + checksum. Shared,
// parameterized by the descriptor (ADR 0010) — not one function per item.
// Bidirectional (ADR 0011). Public API; internal helpers live in codec.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "misbklv/ber.hpp"
#include "misbklv/types.hpp"

namespace misbklv::codec {

using ber::Bytes;

// Big-endian unsigned read over `b` (exposed for callers inspecting raw bytes).
std::uint64_t rd_uint(std::span<const std::byte> b);

// Decode raw item bytes -> typed Value, per the descriptor's kind.
Result<Value> decode(const ItemDescriptor& d, std::span<const std::byte> raw);

// Encode a typed Value -> raw bytes. `len` is the target width — the descriptor's
// canonical length when authoring, or the source length when reserializing.
// Numeric widths must be 1..8 bytes. A Value whose variant does not match
// `d.kind` returns TypeMismatch; an invalid width returns BadLength; a value
// that the requested representation cannot hold returns RangeError.
Result<Bytes> encode(const ItemDescriptor& d, const Value& v, std::size_t len);

// ST 1201 IMAPB float<->int mapping, including structural special values (§7.2.3).
double imapb_decode(const ItemDescriptor& d, std::span<const std::byte> raw);
void imapb_encode(const ItemDescriptor& d, double x, Bytes& out, int len);

// True iff `raw` is an IMAP structural special (top 2 bits set). Only meaningful
// for an IMAPB item — a normal uint/linear value may legitimately have bits 7-6 set.
bool is_imap_special(std::span<const std::byte> raw);

// ST 0601 §6.6 checksum: 16-bit running sum over key..checksum-length.
std::uint16_t bcc16(std::span<const std::byte> data);

}  // namespace misbklv::codec
