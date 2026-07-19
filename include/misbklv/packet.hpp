// SPDX-License-Identifier: Apache-2.0
// KLV packet parse: UL key + BER length + TLV walk (ADR 0005 layer 1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

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

// Walk a bare TLV item sequence (no UL key / outer length). Used for a Local Set
// value, including a nested set (ADR 0005 recursive core). Borrows into `buf`.
Result<std::vector<Item>> parse_items(std::span<const std::byte> buf);

// Parse one KLV packet from the front of `buf`. Borrows into `buf` (ADR 0005).
Result<Packet> parse_packet(std::span<const std::byte> buf);

// Byte length of the complete KLV packet at the front of `buf`, or 0 if `buf`
// does not yet hold a whole packet (need more data). Frames a reassembled stream
// of concatenated packets — used by the media backend (ADR 0013).
std::size_t packet_frame_length(std::span<const std::byte> buf);

}  // namespace misbklv
