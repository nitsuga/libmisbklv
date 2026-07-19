// SPDX-License-Identifier: Apache-2.0
#include "misbklv/packet.hpp"

#include "misbklv/ber.hpp"

namespace misbklv {

Result<std::vector<Item>> parse_items(std::span<const std::byte> buf) {
  std::vector<Item> items;
  std::size_t pos = 0;
  const std::size_t end = buf.size();
  while (pos < end) {
    auto tag = ber::read_oid(buf, pos);
    if (!tag) return Result<std::vector<Item>>::err(tag.error());
    pos += tag->consumed;
    auto ilen = ber::read_length(buf, pos);
    if (!ilen) return Result<std::vector<Item>>::err(ilen.error());
    pos += ilen->consumed;
    if (pos + ilen->value > end)
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
  const std::size_t vstart = 16 + len->consumed;
  const std::size_t end = vstart + len->value;
  if (end > buf.size()) return Result<Packet>::err(Error::Truncated);

  auto items = parse_items(buf.subspan(vstart, len->value));
  if (!items) return Result<Packet>::err(items.error());
  pkt.items = std::move(*items);
  pkt.total_size = end;
  return Result<Packet>::ok(std::move(pkt));
}

std::size_t packet_frame_length(std::span<const std::byte> buf) {
  if (buf.size() < 17) return 0;  // 16-byte key + at least one length byte
  auto len = ber::read_length(buf, 16);
  if (!len) return 0;             // length not yet parseable -> need more data
  const std::size_t total = 16 + len->consumed + len->value;
  return buf.size() >= total ? total : 0;
}

}  // namespace misbklv
