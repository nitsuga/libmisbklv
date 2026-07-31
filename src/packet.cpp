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

bool has_smpte_ul_prefix(std::span<const std::byte> buf) {
  return buf.size() >= std::size(kSmpteUlPrefix) &&
         std::equal(std::begin(kSmpteUlPrefix), std::end(kSmpteUlPrefix), buf.begin());
}

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

std::size_t packet_frame_length(std::span<const std::byte> buf) {
  if (!has_smpte_ul_prefix(buf)) return 0;
  if (buf.size() < 17) return 0;  // 16-byte key + at least one length byte
  auto len = ber::read_length(buf, 16);
  if (!len) return 0;             // length not yet parseable -> need more data
  const std::size_t header = 16 + len->consumed;  // <= buf.size() (read_length)
  // Overflow-safe: a crafted huge length must not wrap `header + value` into a
  // small "complete" total. Not all here (or absurd) -> need more data.
  if (len->value > buf.size() - header) return 0;
  return header + len->value;
}

}  // namespace misbklv
