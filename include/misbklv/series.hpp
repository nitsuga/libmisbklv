// SPDX-License-Identifier: Apache-2.0
// ST 0903 Series type (§9.1.3) + VTarget Pack (§10.2). A Series value is a
// run of length-prefixed elements; each VTarget Pack element is
// [BER-OID targetId][VTarget LS items TLV]. Structural only — the caller decodes
// pack items against the VTarget registry (childRegistry routing, ADR 0010).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "misbklv/ber.hpp"
#include "misbklv/packet.hpp"
#include "misbklv/types.hpp"

namespace misbklv {

struct VTargetPack {
  std::uint64_t target_id;      // BER-OID identifier
  std::vector<Item> items;      // VTarget LS items (borrowed spans)
};

// Parse a vTargetSeries (Item 101) value into its packs.
inline Result<std::vector<VTargetPack>> parse_vtarget_series(
    std::span<const std::byte> buf) {
  std::vector<VTargetPack> packs;
  std::size_t pos = 0;
  while (pos < buf.size()) {
    auto elen = ber::read_length(buf, pos);
    if (!elen) return Result<std::vector<VTargetPack>>::err(elen.error());
    pos += elen->consumed;
    if (pos + elen->value > buf.size())
      return Result<std::vector<VTargetPack>>::err(Error::Truncated);
    auto element = buf.subspan(pos, elen->value);
    pos += elen->value;

    auto tid = ber::read_oid(element, 0);  // element = targetId + LS items
    if (!tid) return Result<std::vector<VTargetPack>>::err(tid.error());
    auto items = parse_items(element.subspan(tid->consumed));
    if (!items) return Result<std::vector<VTargetPack>>::err(items.error());
    packs.push_back({tid->value, std::move(*items)});
  }
  return Result<std::vector<VTargetPack>>::ok(std::move(packs));
}

// Build one VTarget Pack body: BER-OID targetId followed by already-serialized
// LS item bytes (e.g. from LocalSetBuilder::serialize_items()).
inline ber::Bytes build_vtarget_pack(std::uint64_t target_id,
                                     std::span<const std::byte> item_bytes) {
  ber::Bytes out;
  ber::write_oid(out, target_id);
  out.insert(out.end(), item_bytes.begin(), item_bytes.end());
  return out;
}

// Assemble a Series value from element bodies: each is length-prefixed (§9.1.3).
inline ber::Bytes build_series(const std::vector<ber::Bytes>& elements) {
  ber::Bytes out;
  for (const auto& e : elements) {
    ber::write_length(out, e.size());
    out.insert(out.end(), e.begin(), e.end());
  }
  return out;
}

}  // namespace misbklv
