// SPDX-License-Identifier: Apache-2.0
// ST 0903 Series type (§9.1.3) + VTarget Pack (§10.2). A Series value is a run of
// length-prefixed elements; each VTarget Pack element is
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
  std::uint64_t target_id;  // BER-OID identifier
  std::vector<Item> items;  // VTarget LS items (borrowed spans)
};

// Parse a vTargetSeries (Item 101) value into its packs.
Result<std::vector<VTargetPack>> parse_vtarget_series(std::span<const std::byte> buf);

// Build one VTarget Pack body: BER-OID targetId + already-serialized LS item
// bytes (e.g. from LocalSetBuilder::serialize_items()).
ber::Bytes build_vtarget_pack(std::uint64_t target_id, std::span<const std::byte> item_bytes);

// Assemble a Series value from element bodies (each length-prefixed, §9.1.3).
ber::Bytes build_series(const std::vector<ber::Bytes>& elements);

}  // namespace misbklv
