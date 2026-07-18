// SPDX-License-Identifier: Apache-2.0
// KLV packet parse: UL key + BER length + TLV walk (ADR 0005 layer 1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "misbklv/ber.hpp"
#include "misbklv/types.hpp"

namespace misbklv {

// The 16-byte ST 0601 UAS Datalink LS Universal Label.
inline constexpr std::uint8_t kUas0601Key[16] = {
    0x06, 0x0e, 0x2b, 0x34, 0x02, 0x0b, 0x01, 0x01,
    0x0e, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0x00};

struct Item {
  std::uint16_t tag;
  std::span<const std::byte> value;  // borrowed view into the source buffer
};

struct Packet {
  std::span<const std::byte> ul_key;  // 16 bytes
  std::vector<Item> items;            // in wire order
  std::size_t total_size = 0;         // whole packet: key + len + value
};

// Parse one KLV packet from the front of `buf`. Borrows into `buf` (ADR 0005).
inline Result<Packet> parse_packet(std::span<const std::byte> buf) {
  if (buf.size() < 17) return Result<Packet>::err(Error::Truncated);
  Packet pkt;
  pkt.ul_key = buf.subspan(0, 16);

  auto len = ber::read_length(buf, 16);
  if (!len) return Result<Packet>::err(len.error());
  std::size_t pos = 16 + len->consumed;
  const std::size_t end = pos + len->value;
  if (end > buf.size()) return Result<Packet>::err(Error::Truncated);

  while (pos < end) {
    auto tag = ber::read_oid(buf, pos);
    if (!tag) return Result<Packet>::err(tag.error());
    pos += tag->consumed;
    auto ilen = ber::read_length(buf, pos);
    if (!ilen) return Result<Packet>::err(ilen.error());
    pos += ilen->consumed;
    if (pos + ilen->value > end) return Result<Packet>::err(Error::Truncated);
    pkt.items.push_back(
        {static_cast<std::uint16_t>(tag->value), buf.subspan(pos, ilen->value)});
    pos += ilen->value;
  }
  pkt.total_size = end;
  return Result<Packet>::ok(std::move(pkt));
}

}  // namespace misbklv
