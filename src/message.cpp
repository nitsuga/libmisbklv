// SPDX-License-Identifier: Apache-2.0
#include "misbklv/message.hpp"

#include <algorithm>

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

Result<Message> Message::adopt(std::vector<std::byte>&& bytes) {
  // Parse in place and take the buffer only on success, so a failed adopt leaves
  // the caller's vector untouched. swap() (unlike move-assign) guarantees the
  // parsed spans stay valid and refer to m.bytes_, and leaves `bytes` empty.
  auto pkt = parse_packet(bytes);
  if (!pkt) return Result<Message>::err(pkt.error());
  const Registry* reg = registry_by_key(pkt->ul_key);
  if (!reg) return Result<Message>::err(Error::UnknownTag);
  Message m;
  m.bytes_.swap(bytes);
  m.pkt_ = std::move(*pkt);  // spans into m.bytes_ (same heap block as before the swap)
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

bool Message::in_source(std::uint16_t tag) const {
  for (const auto& it : pkt_.items)
    if (it.tag == tag) return true;
  return false;
}

bool Message::has(std::uint16_t tag) const {
  return in_source(tag) || find_edit(tag) != nullptr;
}

std::optional<std::span<const std::byte>> Message::value_of(std::uint16_t tag) const {
  if (const ber::Bytes* e = find_edit(tag)) return std::span<const std::byte>(*e);
  for (const auto& it : pkt_.items)
    if (it.tag == tag) return it.value;
  return std::nullopt;
}

Result<std::monostate> Message::set(std::uint16_t tag, Value v) {
  // Tag 1 is the ST 0601 §6.6 checksum: encode() always recomputes and re-emits
  // it via finalize(). Accepting a set(1, …) had two different meanings — silently
  // dropped on a parsed message, emitted twice on a created one — so reject it on
  // both paths. (Tag 1 is only the checksum for this standalone packet; inside an
  // embedded Local Set it is ordinary data, handled by LocalSetBuilder directly.)
  if (tag == 1) return Result<std::monostate>::err(Error::ReadOnly);
  const ItemDescriptor* d = reg_->find(tag);
  if (!d) return Result<std::monostate>::err(Error::UnknownTag);
  std::size_t len = d->fixed_len;  // new tag: descriptor width
  for (const auto& it : pkt_.items)
    if (it.tag == tag) {
      len = it.value.size();
      break;
    }  // else preserve source width
  auto enc = codec::encode(*d, v, len);
  if (!enc) return Result<std::monostate>::err(enc.error());
  for (auto& e : edits_)
    if (e.first == tag) {
      e.second = std::move(*enc);
      return Result<std::monostate>::ok({});
    }
  edits_.emplace_back(tag, std::move(*enc));
  return Result<std::monostate>::ok({});
}

Result<ber::Bytes> Message::encode() const {
  if (edits_.empty() && pkt_.total_size != 0)
    return Result<ber::Bytes>::ok(ber::Bytes(bytes_.begin(), bytes_.begin() + pkt_.total_size));

  LocalSetBuilder b(*reg_);
  for (const auto& it : pkt_.items) {
    if (it.tag == 1) continue;  // checksum re-emitted by finalize()
    if (const ber::Bytes* e = find_edit(it.tag))
      b.append_raw(it.tag, *e);
    else
      b.append_raw(it.tag, it.value);
  }
  for (const auto& [tag, bytes] : edits_)  // tags added, not present in the source
    if (!in_source(tag)) b.append_raw(tag, bytes);
  // Enforce mandatory items only when authoring a packet from create() (no source
  // bytes). Editing a parsed packet must stay faithful: a stream may legitimately
  // omit items (Report-on-Change), and forcing enforcement would reject
  // re-encoding an already-non-conformant capture after a single edit.
  const bool authoring = bytes_.empty();
  return std::move(b).finalize(reg_->ul_key, /*enforce_mandatory=*/authoring);
}

Result<bool> Message::checksum_valid() const {
  // Only registries whose tag 1 is the 2-byte BCC16 checksum qualify (ST 0601 and
  // standalone ST 0903 share it). Decided from the descriptor, not the registry.
  const ItemDescriptor* d = reg_ ? reg_->find(1) : nullptr;
  if (pkt_.total_size < 2 || !d || d->name != "Checksum" || d->kind != ValueKind::UInt ||
      d->variable || d->fixed_len != 2)
    return Result<bool>::err(Error::UnknownTag);
  const Item* cs = nullptr;
  for (const auto& it : pkt_.items)
    if (it.tag == 1) {
      cs = &it;
      break;
    }
  if (!cs) return Result<bool>::err(Error::UnknownTag);
  const std::byte* end = bytes_.data() + pkt_.total_size;
  if (cs->value.size() != 2 || cs->value.data() + 2 != end)
    return Result<bool>::err(Error::BadLength);
  const std::uint16_t want =
      codec::bcc16(std::span<const std::byte>(bytes_.data(), pkt_.total_size - 2));
  const std::uint16_t got = static_cast<std::uint16_t>(
      (std::to_integer<unsigned>(cs->value[0]) << 8) | std::to_integer<unsigned>(cs->value[1]));
  return Result<bool>::ok(want == got);
}

std::span<const std::byte> Message::original_bytes() const {
  if (pkt_.total_size == 0 || bytes_.size() < pkt_.total_size) return {};
  return std::span<const std::byte>(bytes_.data(), pkt_.total_size);
}

}  // namespace misbklv
