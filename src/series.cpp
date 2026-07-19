// SPDX-License-Identifier: Apache-2.0
#include "misbklv/series.hpp"

namespace misbklv {

Result<std::vector<VTargetPack>> parse_vtarget_series(std::span<const std::byte> buf) {
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

ber::Bytes build_vtarget_pack(std::uint64_t target_id,
                              std::span<const std::byte> item_bytes) {
  ber::Bytes out;
  ber::write_oid(out, target_id);
  out.insert(out.end(), item_bytes.begin(), item_bytes.end());
  return out;
}

ber::Bytes build_series(const std::vector<ber::Bytes>& elements) {
  ber::Bytes out;
  for (const auto& e : elements) {
    ber::write_length(out, e.size());
    out.insert(out.end(), e.begin(), e.end());
  }
  return out;
}

}  // namespace misbklv
