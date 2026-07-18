// SPDX-License-Identifier: Apache-2.0
// Value codecs (raw-int / linear-LDS / utf8) + checksum. Shared, parameterized
// by the descriptor (ADR 0010) — not one function per item. Bidirectional (ADR 0011).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <variant>

#include "misbklv/ber.hpp"
#include "misbklv/types.hpp"

namespace misbklv::codec {

using ber::Bytes;

// --- big-endian integer read/write -----------------------------------------
inline std::uint64_t rd_uint(std::span<const std::byte> b) {
  std::uint64_t v = 0;
  for (auto by : b) v = (v << 8) | std::to_integer<std::uint8_t>(by);
  return v;
}
inline std::int64_t rd_int(std::span<const std::byte> b) {
  std::uint64_t u = rd_uint(b);
  const int bits = 8 * static_cast<int>(b.size());
  if (bits < 64 && ((u >> (bits - 1)) & 1)) u |= (~0ull << bits);
  return static_cast<std::int64_t>(u);
}
inline void wr_uint(Bytes& out, std::uint64_t v, int len) {
  for (int i = len - 1; i >= 0; --i)
    out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}

// --- linear LDS map (legacy 0601; symmetric-signed or unsigned-affine) ------
inline double linear_decode(const ItemDescriptor& d, std::span<const std::byte> raw) {
  const int len = static_cast<int>(raw.size());
  if (d.is_signed) {
    const double imax = static_cast<double>((1ll << (8 * len - 1)) - 1);
    return static_cast<double>(rd_int(raw)) * d.map.max / imax;  // min == -max
  }
  const double denom =
      (len >= 8) ? 18446744073709551615.0 : static_cast<double>((1ull << (8 * len)) - 1);
  return d.map.min + static_cast<double>(rd_uint(raw)) * (d.map.max - d.map.min) / denom;
}
inline void linear_encode(const ItemDescriptor& d, double x, Bytes& out, int len) {
  if (d.is_signed) {
    const double imax = static_cast<double>((1ll << (8 * len - 1)) - 1);
    const auto v = static_cast<std::int64_t>(std::llround(x * imax / d.map.max));
    wr_uint(out, static_cast<std::uint64_t>(v), len);
  } else {
    const double denom =
        (len >= 8) ? 18446744073709551615.0 : static_cast<double>((1ull << (8 * len)) - 1);
    const auto v = static_cast<std::uint64_t>(
        std::llround((x - d.map.min) * denom / (d.map.max - d.map.min)));
    wr_uint(out, v, len);
  }
}

// --- ST 1201 IMAPB (Starting Point B): float [a,b] <-> unsigned L-byte int ---
struct ImapB {
  double sF, sR, zoff, a;
};
inline ImapB imapb_params(double a, double b, int len) {
  const double bPow = std::ceil(std::log2(b - a));  // ST 1201 §8, eq. bPow
  const double dPow = 8.0 * len - 1.0;
  ImapB p;
  p.sF = std::exp2(dPow - bPow);                     // forward scaling factor
  p.sR = std::exp2(bPow - dPow);                     // reverse scaling factor
  p.zoff = (a < 0.0 && b > 0.0) ? (p.sF * a - std::floor(p.sF * a)) : 0.0;
  p.a = a;
  return p;
}
// IMAP special values (ST 1201 §7.2.3, Tables 2-3). A value is special iff its
// top two bits are both 1; the identifier lives in the high bits of byte 0,
// remaining bytes zero-filled.
enum class ImapSpecial { None, PosInf, NegInf, NaNv, BelowMin, AboveMax, Reserved };

inline ImapSpecial imapb_special_of(std::uint8_t b0) {
  if ((b0 & 0xC0) != 0xC0) return ImapSpecial::None;  // top 2 bits not both set
  switch (b0 >> 3) {                                  // top 5 bits (Table 2)
    case 0x19: return ImapSpecial::PosInf;            // 11001
    case 0x1D: return ImapSpecial::NegInf;            // 11101
    case 0x1A: case 0x1E: case 0x1B: case 0x1F:       // +-QNaN / +-SNaN
      return ImapSpecial::NaNv;
    case 0x1C:                                        // 11100 -> MISB (Table 3)
      return (b0 & 0x01) ? ImapSpecial::AboveMax : ImapSpecial::BelowMin;
    default:
      return ImapSpecial::Reserved;
  }
}

// True iff `raw` is any IMAP special (top 2 bits set). Only meaningful for an
// IMAPB-typed item — a normal uint/linear value may legitimately have bits 7-6 set.
inline bool is_imap_special(std::span<const std::byte> raw) {
  return !raw.empty() && (std::to_integer<std::uint8_t>(raw[0]) & 0xC0) == 0xC0;
}

inline void imapb_put_special(Bytes& out, std::uint8_t id, int len) {
  out.push_back(static_cast<std::byte>(id));
  for (int i = 1; i < len; ++i) out.push_back(std::byte{0});
}

inline double imapb_decode(const ItemDescriptor& d, std::span<const std::byte> raw) {
  if (!raw.empty()) {
    switch (imapb_special_of(std::to_integer<std::uint8_t>(raw[0]))) {
      case ImapSpecial::PosInf: return std::numeric_limits<double>::infinity();
      case ImapSpecial::NegInf: return -std::numeric_limits<double>::infinity();
      case ImapSpecial::NaNv:
      case ImapSpecial::Reserved:  // forward-compat: surface as NaN, don't crash
        return std::numeric_limits<double>::quiet_NaN();
      case ImapSpecial::BelowMin: return d.map.min;  // ST 1201 default reverse
      case ImapSpecial::AboveMax: return d.map.max;
      case ImapSpecial::None: break;
    }
  }
  const ImapB p = imapb_params(d.map.min, d.map.max, static_cast<int>(raw.size()));
  return p.sR * (static_cast<double>(rd_uint(raw)) - p.zoff) + p.a;
}

inline void imapb_encode(const ItemDescriptor& d, double x, Bytes& out, int len) {
  if (std::isnan(x)) return imapb_put_special(out, 0xD0, len);              // +QNaN
  if (std::isinf(x)) return imapb_put_special(out, x > 0 ? 0xC8 : 0xE8, len);
  if (x < d.map.min) return imapb_put_special(out, 0xE0, len);             // BELOW_MIN
  if (x > d.map.max) return imapb_put_special(out, 0xE1, len);             // ABOVE_MAX
  const ImapB p = imapb_params(d.map.min, d.map.max, len);
  double yf = p.sF * (x - p.a) + p.zoff;             // truncate; x>=a => floor
  // A decoded on-grid value re-encodes to an exact integer in real arithmetic;
  // float error can pull it just below, so floor would drop 1 LSB. Nudge up by
  // a few ULP (far below a real cell width) to keep IMAPB a stable inverse.
  yf += (std::fabs(yf) + 1.0) * 8.0 * std::numeric_limits<double>::epsilon();
  wr_uint(out, static_cast<std::uint64_t>(std::floor(yf)), len);
}

// --- decode raw bytes -> typed Value ---------------------------------------
inline Result<Value> decode(const ItemDescriptor& d, std::span<const std::byte> raw) {
  switch (d.kind) {
    case ValueKind::UInt:
      return Result<Value>::ok(Value{rd_uint(raw)});
    case ValueKind::Int:
      return Result<Value>::ok(Value{rd_int(raw)});
    case ValueKind::LinearLDS:
      return Result<Value>::ok(Value{linear_decode(d, raw)});
    case ValueKind::IMAPB:
      return Result<Value>::ok(Value{imapb_decode(d, raw)});
    case ValueKind::Utf8:
      return Result<Value>::ok(Value{std::string_view{
          reinterpret_cast<const char*>(raw.data()), raw.size()}});
    default:
      return Result<Value>::ok(Value{raw});
  }
}

// --- encode typed Value -> raw value bytes ---------------------------------
// `len` is the target byte width for fixed/mapped kinds. Callers pass the
// descriptor's canonical length when authoring, or the source length when
// reserializing an existing item (0601 has variable-length items; ADR 0011).
inline Result<Bytes> encode(const ItemDescriptor& d, const Value& v, std::size_t len) {
  Bytes out;
  switch (d.kind) {
    case ValueKind::UInt:
      wr_uint(out, std::get<std::uint64_t>(v), static_cast<int>(len));
      return Result<Bytes>::ok(std::move(out));
    case ValueKind::Int:
      wr_uint(out, static_cast<std::uint64_t>(std::get<std::int64_t>(v)),
              static_cast<int>(len));
      return Result<Bytes>::ok(std::move(out));
    case ValueKind::LinearLDS:
      linear_encode(d, std::get<double>(v), out, static_cast<int>(len));
      return Result<Bytes>::ok(std::move(out));
    case ValueKind::IMAPB:
      imapb_encode(d, std::get<double>(v), out, static_cast<int>(len));
      return Result<Bytes>::ok(std::move(out));
    case ValueKind::Utf8: {
      auto sv = std::get<std::string_view>(v);
      for (char c : sv) out.push_back(static_cast<std::byte>(c));
      return Result<Bytes>::ok(std::move(out));
    }
    default: {
      auto sp = std::get<std::span<const std::byte>>(v);
      out.assign(sp.begin(), sp.end());
      return Result<Bytes>::ok(std::move(out));
    }
  }
}

// --- ST 0601 §6.6 checksum: 16-bit running sum over key..checksum-length ----
inline std::uint16_t bcc16(std::span<const std::byte> data) {
  std::uint16_t sum = 0;
  for (std::size_t i = 0; i < data.size(); ++i)
    sum += static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data[i]))
           << (8 * ((i + 1) % 2));
  return sum;
}

}  // namespace misbklv::codec
