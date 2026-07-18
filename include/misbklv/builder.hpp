// SPDX-License-Identifier: Apache-2.0
// LocalSetBuilder — owned, bottom-up serialization (ADR 0011). A value is fully
// encoded before its length is written, so BER lengths are never back-patched.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "misbklv/ber.hpp"
#include "misbklv/codec.hpp"
#include "misbklv/types.hpp"

namespace misbklv {

class LocalSetBuilder {
 public:
  explicit LocalSetBuilder(const Registry& reg) : reg_(reg) {}

  // Encode a typed value for `tag` per its descriptor (forward codec) and stage it.
  // `len` overrides the target byte width — pass the source length to reserialize
  // a variable-length or non-canonically-sized item byte-exactly (ADR 0011).
  Result<std::monostate> set(std::uint16_t tag, const Value& v,
                             std::optional<std::size_t> len = std::nullopt) {
    const ItemDescriptor* d = reg_.find(tag);
    if (!d) return Result<std::monostate>::err(Error::UnknownTag);
    auto enc = codec::encode(*d, v, len.value_or(d->fixed_len));
    if (!enc) return Result<std::monostate>::err(enc.error());
    staged_.emplace_back(tag, std::move(*enc));
    return Result<std::monostate>::ok({});
  }

  // Escape hatch: stage pre-encoded value bytes (unregistered / opaque items).
  void append_raw(std::uint16_t tag, std::span<const std::byte> bytes) {
    staged_.emplace_back(tag, ber::Bytes(bytes.begin(), bytes.end()));
  }

  // Serialize the staged items as a bare TLV sequence — no UL key, length, or
  // checksum. This is the value of a nested Local Set (ADR 0005/0010): the
  // parent stages it via append_raw(child_tag, serialize_items()).
  // Callers exclude the checksum item before staging (finalize() emits a fresh
  // one); tag 1 is NOT special here — in embedded LSs (e.g. VTarget) it is data.
  ber::Bytes serialize_items() const {
    ber::Bytes out;
    for (const auto& [tag, val] : staged_) {
      ber::write_oid(out, tag);
      ber::write_length(out, val.size());
      out.insert(out.end(), val.begin(), val.end());
    }
    return out;
  }

  // Assemble the full packet: key + BER len + items + Item 1 checksum (last).
  // `enforce_mandatory` validates required items when authoring a new packet;
  // pass false to faithfully reserialize an existing (e.g. Report-on-Change)
  // packet that legitimately omits items. (ADR 0011 — see reserialize note.)
  Result<ber::Bytes> finalize(std::span<const std::uint8_t> ul_key,
                              bool enforce_mandatory = true) && {
    // 1. mandatory-item check (ADR 0011).
    if (enforce_mandatory)
      for (const auto& d : reg_.items)
        if ((d.flags & kMandatory) && !staged_contains(d.tag))
          return Result<ber::Bytes>::err(Error::MissingMandatory);

    // 2. serialize staged items (skip any staged checksum — we emit it).
    ber::Bytes items = serialize_items();

    // 3. checksum item is tag(1) + len(1) + value(2) = 4 bytes of the value field.
    const std::uint64_t value_len = items.size() + 4;

    ber::Bytes pkt;
    for (auto b : ul_key) pkt.push_back(static_cast<std::byte>(b));
    ber::write_length(pkt, value_len);
    pkt.insert(pkt.end(), items.begin(), items.end());
    ber::write_oid(pkt, kChecksumTag);                 // 0x01
    ber::write_length(pkt, 2);                          // 0x02

    // 4. 16-bit BCC over key..checksum-length, then append the value (ST 0601 §6.6).
    const std::uint16_t cs = codec::bcc16(pkt);
    pkt.push_back(static_cast<std::byte>((cs >> 8) & 0xFF));
    pkt.push_back(static_cast<std::byte>(cs & 0xFF));
    return Result<ber::Bytes>::ok(std::move(pkt));
  }

 private:
  static constexpr std::uint16_t kChecksumTag = 1;

  bool staged_contains(std::uint16_t tag) const {
    for (const auto& s : staged_)
      if (s.first == tag) return true;
    return false;
  }

  const Registry& reg_;
  std::vector<std::pair<std::uint16_t, ber::Bytes>> staged_;
};

}  // namespace misbklv
