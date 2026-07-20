// SPDX-License-Identifier: Apache-2.0
#include "misbklv/message.hpp"

#include "misbklv/builder.hpp"

namespace misbklv {

Result<Message> Message::parse(std::span<const std::byte> bytes) {
  Message m;
  m.bytes_.assign(bytes.begin(), bytes.end());
  auto pkt = parse_packet(m.bytes_);
  if (!pkt) return Result<Message>::err(pkt.error());
  m.pkt_ = std::move(*pkt);  // spans into m.bytes_ (preserved across the move)
  const Registry* reg = registry_by_key(m.pkt_.ul_key);
  if (!reg) return Result<Message>::err(Error::UnknownTag);  // unknown packet type
  m.reg_ = reg;
  return Result<Message>::ok(std::move(m));
}

Result<Message> Message::create(RegistryId id) {
  const Registry* reg = registry_for(id);
  if (!reg || reg->ul_key.empty())  // not a standalone packet type
    return Result<Message>::err(Error::Unsupported);
  Message m;  // empty: no source bytes/items; set() populates edits_, encode() emits
  m.reg_ = reg;
  return Result<Message>::ok(std::move(m));
}

const ber::Bytes* Message::find_edit(std::uint16_t tag) const {
  for (const auto& e : edits_)
    if (e.first == tag) return &e.second;
  return nullptr;
}

bool Message::has(std::uint16_t tag) const {
  for (const auto& it : pkt_.items)
    if (it.tag == tag) return true;
  return false;
}

std::optional<std::span<const std::byte>> Message::value_of(std::uint16_t tag) const {
  if (const ber::Bytes* e = find_edit(tag)) return std::span<const std::byte>(*e);
  for (const auto& it : pkt_.items)
    if (it.tag == tag) return it.value;
  return std::nullopt;
}

Result<std::monostate> Message::set(std::uint16_t tag, Value v) {
  const ItemDescriptor* d = reg_->find(tag);
  if (!d) return Result<std::monostate>::err(Error::UnknownTag);
  std::size_t len = d->fixed_len;  // new tag: descriptor width
  for (const auto& it : pkt_.items)
    if (it.tag == tag) { len = it.value.size(); break; }  // else preserve source width
  auto enc = codec::encode(*d, v, len);
  if (!enc) return Result<std::monostate>::err(enc.error());
  for (auto& e : edits_)
    if (e.first == tag) { e.second = std::move(*enc); return Result<std::monostate>::ok({}); }
  edits_.emplace_back(tag, std::move(*enc));
  return Result<std::monostate>::ok({});
}

Result<ber::Bytes> Message::encode() const {
  LocalSetBuilder b(*reg_);
  for (const auto& it : pkt_.items) {
    if (it.tag == 1) continue;  // checksum re-emitted by finalize()
    if (const ber::Bytes* e = find_edit(it.tag))
      b.append_raw(it.tag, *e);
    else
      b.append_raw(it.tag, it.value);
  }
  for (const auto& [tag, bytes] : edits_)  // tags added, not present in the source
    if (!has(tag)) b.append_raw(tag, bytes);
  return std::move(b).finalize(reg_->ul_key, /*enforce_mandatory=*/false);
}

}  // namespace misbklv
