// SPDX-License-Identifier: Apache-2.0
// Message — the high-level, owned, editable view of one KLV packet (ADR 0018).
// The owns-for-convenience layer above the borrow-based core (ADR 0011): parse
// copies the bytes, typed get<T>/set decode/encode via the registry, and encode()
// rebuilds byte-exact for untouched items. Core-only (no gstreamer).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "misbklv/backend.hpp"  // kNoPts
#include "misbklv/ber.hpp"
#include "misbklv/codec.hpp"
#include "misbklv/packet.hpp"
#include "misbklv/registries.hpp"
#include "misbklv/types.hpp"

namespace misbklv {

class Message {
 public:
  // Copy + parse one KLV packet; the registry is chosen from the UL key.
  static Result<Message> parse(std::span<const std::byte> bytes);

  // Move-adopt an already-owned packet buffer; parses in place without copying.
  // Transfers ownership of `bytes` into the Message (caller’s vector is empty
  // on success). Preserves `parse` copy semantics for public API.
  static Result<Message> adopt(std::vector<std::byte>&& bytes);

  // Author a fresh, empty packet for `registry` — ST 0601 (`RegistryId::Uas0601`)
  // or standalone ST 0903 VMTI (`RegistryId::Vmti0903`). Populate with `set()`,
  // then `encode()` / emit. Errors for a type with no standalone UL key (e.g.
  // `Vtarget0903`, a pack — build those with `series.hpp` / `LocalSetBuilder`).
  static Result<Message> create(RegistryId registry);

  const Registry& registry() const { return *reg_; }
  const std::vector<Item>& items() const { return pkt_.items; }  // wire order
  // True if `tag` occurs in the parsed source packet or has a staged set() edit.
  bool has(std::uint16_t tag) const;

  // Named-tag overload: has(tags::Uas0601::SensorLatitude).
  template <class E, std::enable_if_t<std::is_enum_v<E> &&
                                          std::is_same_v<std::underlying_type_t<E>, std::uint16_t>,
                                      int> = 0>
  bool has(E tag) const {
    return has(static_cast<std::uint16_t>(tag));
  }

  // Typed read: decodes the item (edited value if set()) and returns it iff the
  // requested type matches the descriptor's ValueKind, else nullopt.
  template <class T> std::optional<T> get(std::uint16_t tag) const {
    const ItemDescriptor* d = reg_->find(tag);
    if (!d) return std::nullopt;
    const auto raw = value_of(tag);
    if (!raw) return std::nullopt;
    auto v = codec::decode(*d, *raw);
    if (!v) return std::nullopt;
    if (const T* p = std::get_if<T>(&*v)) return *p;
    return std::nullopt;
  }

  // Named-tag overload: get<double>(tags::Uas0601::SensorLatitude) etc. Accepts
  // the generated per-registry tag enums (ADR 0018 follow-on); numbers still work.
  template <
      class T, class E,
      std::enable_if_t<
          std::is_enum_v<E> && std::is_same_v<std::underlying_type_t<E>, std::uint16_t>, int> = 0>
  std::optional<T> get(E tag) const {
    return get<T>(static_cast<std::uint16_t>(tag));
  }

  // Stage a typed edit (encoded at the item's source width, or the descriptor
  // width for a new tag). Applied by encode().
  Result<std::monostate> set(std::uint16_t tag, Value v);

  // Named-tag overload: set(tags::Uas0601::SensorLatitude, Value{...}).
  template <class E, std::enable_if_t<std::is_enum_v<E> &&
                                          std::is_same_v<std::underlying_type_t<E>, std::uint16_t>,
                                      int> = 0>
  Result<std::monostate> set(E tag, Value v) {
    return set(static_cast<std::uint16_t>(tag), std::move(v));
  }

  // With no staged edits on a parsed Message, returns an owned byte-for-byte copy
  // of that packet only (excluding trailing parse input). Otherwise rebuilds:
  // untouched items pass through byte-exact, edited/added via the codec, and the
  // checksum is re-emitted.
  Result<ber::Bytes> encode() const;

  // True if any `set()` has staged an edit/append.
  bool edited() const { return !edits_.empty(); }

  // Original packet extent as owned by this Message (byte-exact source slice).
  // Empty if this Message was created via `create()` with no source.
  std::span<const std::byte> original_bytes() const;

  std::int64_t pts() const { return pts_; }
  void set_pts(std::int64_t p) { pts_ = p; }

  Message(Message&&) = default;
  Message& operator=(Message&&) = default;
  Message(const Message&) = delete;  // spans borrow bytes_; move preserves it
  Message& operator=(const Message&) = delete;

 private:
  Message() = default;                     // parse() is the entry point
  template <class U> friend class Result;  // Result<Message> value-inits on err()
  std::optional<std::span<const std::byte>> value_of(std::uint16_t tag) const;
  const ber::Bytes* find_edit(std::uint16_t tag) const;
  bool in_source(std::uint16_t tag) const;

  std::vector<std::byte> bytes_;  // owned source copy; pkt_ spans into it
  Packet pkt_;
  const Registry* reg_ = nullptr;
  std::vector<std::pair<std::uint16_t, ber::Bytes>> edits_;
  std::int64_t pts_ = kNoPts;
};

}  // namespace misbklv
