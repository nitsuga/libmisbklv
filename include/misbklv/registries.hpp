// SPDX-License-Identifier: Apache-2.0
// Registry resolver: maps a RegistryId (a descriptor's `child`) to its table,
// so the recursive core can descend into nested Local Sets (ADR 0010).
#pragma once

#include <cstddef>
#include <span>

#include "misbklv/types.hpp"
#include "registry/uas0601_tables.generated.hpp"
#include "registry/vmti0903_tables.generated.hpp"

namespace misbklv {

// All top-level registries (those with a 16-byte UL key). Nested-only sets need
// not appear here.
inline constexpr const Registry* kRegistries[] = {&gen::uas_0601, &gen::vmti_0903};

// Resolve a nested item's child (ADR 0010 childRegistry routing).
inline const Registry* registry_for(RegistryId id) {
  switch (id) {
    case RegistryId::Uas0601:
      return &gen::uas_0601;
    case RegistryId::Vmti0903:
      return &gen::vmti_0903;
    case RegistryId::None:
    default:
      return nullptr;
  }
}

// Select a registry by a standalone packet's 16-byte UL key (demux dispatch):
// this is how the standalone-VMTI vs 0601 variants are told apart on the wire.
inline const Registry* registry_by_key(std::span<const std::byte> key) {
  for (const Registry* r : kRegistries) {
    if (r->ul_key.size() != key.size()) continue;
    bool eq = true;
    for (std::size_t i = 0; i < key.size(); ++i)
      if (r->ul_key[i] != std::to_integer<std::uint8_t>(key[i])) {
        eq = false;
        break;
      }
    if (eq) return r;
  }
  return nullptr;
}

}  // namespace misbklv
