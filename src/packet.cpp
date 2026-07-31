// SPDX-License-Identifier: Apache-2.0
#include "misbklv/packet.hpp"

#include <algorithm>
#include <iterator>
#include <limits>

#include "misbklv/ber.hpp"

namespace misbklv {
namespace {

constexpr std::byte kSmpteUlPrefix[] = {
    std::byte{0x06}, std::byte{0x0e}, std::byte{0x2b}, std::byte{0x34}};

}  // namespace

Result<std::vector<Item>> parse_items(std::span<const std::byte> buf) {
  std::vector<Item> items;
  std::size_t pos = 0;
  const std::size_t end = buf.size();
  while (pos < end) {
    auto tag = ber::read_oid(buf, pos);
    if (!tag) return Result<std::vector<Item>>::err(tag.error());
    if (tag->value > std::numeric_limits<std::uint16_t>::max())
      return Result<std::vector<Item>>::err(Error::OutOfRange);
    pos += tag->consumed;
    auto ilen = ber::read_length(buf, pos);
    if (!ilen) return Result<std::vector<Item>>::err(ilen.error());
    pos += ilen->consumed;
    // Overflow-safe: `pos + ilen->value` could wrap for a crafted huge length
    // (8-byte BER len), passing a naive `> end` check and then over-reading in
    // subspan. `pos <= end` here, so `end - pos` can't underflow.
    if (ilen->value > end - pos)
      return Result<std::vector<Item>>::err(Error::Truncated);
    items.push_back(
        {static_cast<std::uint16_t>(tag->value), buf.subspan(pos, ilen->value)});
    pos += ilen->value;
  }
  return Result<std::vector<Item>>::ok(std::move(items));
}

Result<Packet> parse_packet(std::span<const std::byte> buf) {
  if (buf.size() < 17) return Result<Packet>::err(Error::Truncated);
  Packet pkt;
  pkt.ul_key = buf.subspan(0, 16);

  auto len = ber::read_length(buf, 16);
  if (!len) return Result<Packet>::err(len.error());
  const std::size_t vstart = 16 + len->consumed;  // <= buf.size() (read_length)
  // Overflow-safe bound: `vstart + len->value` could wrap for a crafted huge
  // length and slip past a naive `> buf.size()` check into an OOB subspan.
  if (len->value > buf.size() - vstart) return Result<Packet>::err(Error::Truncated);
  const std::size_t end = vstart + len->value;

  auto items = parse_items(buf.subspan(vstart, len->value));
  if (!items) return Result<Packet>::err(items.error());
  pkt.items = std::move(*items);
  pkt.total_size = end;
  return Result<Packet>::ok(std::move(pkt));
}

Result<std::optional<std::size_t>> inspect_packet_frame(
    std::span<const std::byte> buf, std::size_t max_packet_bytes) {
  const std::size_t prefix_bytes =
      std::min<std::size_t>(buf.size(), std::size(kSmpteUlPrefix));
  for (std::size_t i = 0; i < prefix_bytes; ++i)
    if (buf[i] != kSmpteUlPrefix[i])
      return Result<std::optional<std::size_t>>::err(Error::BadLength);
  if (prefix_bytes < std::size(kSmpteUlPrefix))
    return Result<std::optional<std::size_t>>::ok(std::nullopt);

  if (buf.size() < 17)
    return Result<std::optional<std::size_t>>::ok(std::nullopt);

  const auto first_length_byte = std::to_integer<std::uint8_t>(buf[16]);
  std::size_t length_bytes = 0;
  std::uint64_t value_length = 0;
  if (first_length_byte < 0x80) {
    value_length = first_length_byte;
  } else {
    length_bytes = first_length_byte & 0x7F;
    if (length_bytes == 0 || length_bytes > 8)
      return Result<std::optional<std::size_t>>::err(Error::BadLength);
    if (buf.size() - 17 < length_bytes)
      return Result<std::optional<std::size_t>>::ok(std::nullopt);
    for (std::size_t i = 0; i < length_bytes; ++i)
      value_length = (value_length << 8) |
                     std::to_integer<std::uint8_t>(buf[17 + i]);
  }

  const std::size_t header_size = 17 + length_bytes;
  if (header_size > max_packet_bytes ||
      value_length > max_packet_bytes - header_size)
    return Result<std::optional<std::size_t>>::err(Error::ResourceLimit);
  const std::size_t total_size = header_size + static_cast<std::size_t>(value_length);
  if (total_size > buf.size())
    return Result<std::optional<std::size_t>>::ok(std::nullopt);
  return Result<std::optional<std::size_t>>::ok(total_size);
}

std::size_t packet_frame_length(std::span<const std::byte> buf) {
  auto frame = inspect_packet_frame(buf, std::numeric_limits<std::size_t>::max());
  return frame && *frame ? **frame : 0;
}

}  // namespace misbklv
