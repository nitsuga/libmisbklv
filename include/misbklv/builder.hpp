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
#include "misbklv/types.hpp"

namespace misbklv {

class LocalSetBuilder {
 public:
  explicit LocalSetBuilder(const Registry& reg) : reg_(reg) {}

  // Encode a typed value for `tag` per its descriptor (forward codec) and stage it.
  // `len` overrides the target byte width — pass the source length to reserialize
  // a variable-length or non-canonically-sized item byte-exactly (ADR 0011).
  Result<std::monostate> set(std::uint16_t tag, const Value& v,
                             std::optional<std::size_t> len = std::nullopt);

  // Escape hatch: stage pre-encoded value bytes (unregistered / opaque items).
  void append_raw(std::uint16_t tag, std::span<const std::byte> bytes);

  // Serialize the staged items as a bare TLV sequence — no UL key, length, or
  // checksum. This is the value of a nested Local Set (ADR 0005/0010): the parent
  // stages it via append_raw(child_tag, serialize_items()).
  ber::Bytes serialize_items() const;

  // Assemble the full packet: key + BER len + items + Item 1 checksum (last).
  // `enforce_mandatory` validates required items when authoring; pass false to
  // faithfully reserialize a packet that legitimately omits items (ADR 0011).
  Result<ber::Bytes> finalize(std::span<const std::uint8_t> ul_key,
                              bool enforce_mandatory = true) &&;

 private:
  static constexpr std::uint16_t kChecksumTag = 1;
  bool staged_contains(std::uint16_t tag) const;

  const Registry& reg_;
  std::vector<std::pair<std::uint16_t, ber::Bytes>> staged_;
};

}  // namespace misbklv
