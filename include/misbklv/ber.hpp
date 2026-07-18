// SPDX-License-Identifier: Apache-2.0
// BER-OID tag + BER length codec (ADR 0005 layer 1; ST 0107 §6.3).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "misbklv/types.hpp"

namespace misbklv::ber {

using Bytes = std::vector<std::byte>;

struct Parsed {
  std::uint64_t value;
  std::size_t consumed;
};

// BER-OID tag: base-128, big-endian, high bit = continuation (ST 0107 §6.3.1).
inline Result<Parsed> read_oid(std::span<const std::byte> buf, std::size_t pos) {
  std::uint64_t v = 0;
  std::size_t n = 0;
  while (pos + n < buf.size()) {
    auto b = std::to_integer<std::uint8_t>(buf[pos + n]);
    v = (v << 7) | (b & 0x7F);
    ++n;
    if ((b & 0x80) == 0) return Result<Parsed>::ok({v, n});
  }
  return Result<Parsed>::err(Error::Truncated);
}

inline void write_oid(Bytes& out, std::uint64_t tag) {
  std::byte tmp[10];
  int i = 0;
  tmp[i++] = static_cast<std::byte>(tag & 0x7F);
  tag >>= 7;
  while (tag) {
    tmp[i++] = static_cast<std::byte>((tag & 0x7F) | 0x80);
    tag >>= 7;
  }
  while (i > 0) out.push_back(tmp[--i]);  // most-significant first
}

// BER length: short form (<0x80) or long form (0x8N + N count bytes). ST 0107 §6.3.2.
inline Result<Parsed> read_length(std::span<const std::byte> buf, std::size_t pos) {
  if (pos >= buf.size()) return Result<Parsed>::err(Error::Truncated);
  auto b = std::to_integer<std::uint8_t>(buf[pos]);
  if (b < 0x80) return Result<Parsed>::ok({b, 1});
  std::size_t n = b & 0x7F;
  if (n == 0 || n > 8 || pos + 1 + n > buf.size())
    return Result<Parsed>::err(Error::BadLength);
  std::uint64_t v = 0;
  for (std::size_t k = 0; k < n; ++k)
    v = (v << 8) | std::to_integer<std::uint8_t>(buf[pos + 1 + k]);
  return Result<Parsed>::ok({v, 1 + n});
}

// Encode length in the fewest bytes (ST 0107 3-05).
inline void write_length(Bytes& out, std::uint64_t len) {
  if (len < 0x80) {
    out.push_back(static_cast<std::byte>(len));
    return;
  }
  std::byte tmp[8];
  int n = 0;
  while (len) {
    tmp[n++] = static_cast<std::byte>(len & 0xFF);
    len >>= 8;
  }
  out.push_back(static_cast<std::byte>(0x80 | n));
  while (n > 0) out.push_back(tmp[--n]);  // big-endian
}

}  // namespace misbklv::ber
