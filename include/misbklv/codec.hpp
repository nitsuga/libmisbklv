// SPDX-License-Identifier: Apache-2.0
// Value codecs (raw-int / linear-LDS / utf8) + checksum. Shared, parameterized
// by the descriptor (ADR 0010) — not one function per item. Bidirectional (ADR 0011).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
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
inline void linear_encode(const ItemDescriptor& d, double x, Bytes& out) {
  const int len = d.fixed_len;
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

// --- decode raw bytes -> typed Value ---------------------------------------
inline Result<Value> decode(const ItemDescriptor& d, std::span<const std::byte> raw) {
  switch (d.kind) {
    case ValueKind::UInt:
      return Result<Value>::ok(Value{rd_uint(raw)});
    case ValueKind::Int:
      return Result<Value>::ok(Value{rd_int(raw)});
    case ValueKind::LinearLDS:
    case ValueKind::IMAPB:
      return Result<Value>::ok(Value{linear_decode(d, raw)});
    case ValueKind::Utf8:
      return Result<Value>::ok(Value{std::string_view{
          reinterpret_cast<const char*>(raw.data()), raw.size()}});
    default:
      return Result<Value>::ok(Value{raw});
  }
}

// --- encode typed Value -> raw value bytes ---------------------------------
inline Result<Bytes> encode(const ItemDescriptor& d, const Value& v) {
  Bytes out;
  switch (d.kind) {
    case ValueKind::UInt:
      wr_uint(out, std::get<std::uint64_t>(v), d.fixed_len);
      return Result<Bytes>::ok(std::move(out));
    case ValueKind::Int:
      wr_uint(out, static_cast<std::uint64_t>(std::get<std::int64_t>(v)), d.fixed_len);
      return Result<Bytes>::ok(std::move(out));
    case ValueKind::LinearLDS:
    case ValueKind::IMAPB:
      linear_encode(d, std::get<double>(v), out);
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
