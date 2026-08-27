// SPDX-License-Identifier: Apache-2.0
#include "misbklv/ber.hpp"

#include <limits>

namespace misbklv::ber {

Result<Parsed> read_oid(std::span<const std::byte> buf, std::size_t pos) {
  if (pos >= buf.size()) return Result<Parsed>::err(Error::Truncated);

  std::uint64_t v = 0;
  std::size_t cursor = pos;
  while (cursor < buf.size()) {
    auto b = std::to_integer<std::uint8_t>(buf[cursor]);
    if (cursor == pos && b == 0x80) return Result<Parsed>::err(Error::BadLength);

    const auto payload = static_cast<std::uint64_t>(b & 0x7F);
    if (v > (std::numeric_limits<std::uint64_t>::max() - payload) / 128)
      return Result<Parsed>::err(Error::OutOfRange);
    v = (v << 7) | payload;
    ++cursor;
    if ((b & 0x80) == 0) return Result<Parsed>::ok({v, cursor - pos});
  }
  return Result<Parsed>::err(Error::Truncated);
}

void write_oid(Bytes& out, std::uint64_t tag) {
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

Result<Parsed> read_length(std::span<const std::byte> buf, std::size_t pos) {
  if (pos >= buf.size()) return Result<Parsed>::err(Error::Truncated);
  auto b = std::to_integer<std::uint8_t>(buf[pos]);
  if (b < 0x80) return Result<Parsed>::ok({b, 1});
  std::size_t n = b & 0x7F;
  if (n == 0 || n > 8 || pos + 1 + n > buf.size()) return Result<Parsed>::err(Error::BadLength);
  // Non-minimal long forms (`0x81 0x05`, `0x82 0x00 0x05`) are accepted, not
  // rejected: ST 0107 §6.3.2 prescribes the fewest-bytes form, but leniency here
  // is safe — write_length() always emits the minimal form, so we never produce
  // one, and decode->encode canonicalizes an over-long input rather than
  // preserving it. A robust reader over a strict writer (ADR 0030).
  std::uint64_t v = 0;
  for (std::size_t k = 0; k < n; ++k)
    v = (v << 8) | std::to_integer<std::uint8_t>(buf[pos + 1 + k]);
  return Result<Parsed>::ok({v, 1 + n});
}

void write_length(Bytes& out, std::uint64_t len) {
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
