// SPDX-License-Identifier: Apache-2.0
// Registry resolvers: childRegistry routing (ADR 0010) and UL-key demux dispatch.
// The generated constexpr tables (gen::*) are included here so callers can use
// them directly; the resolver logic is compiled into the library.
#pragma once

#include <cstddef>
#include <span>

#include "misbklv/types.hpp"
#include "registry/uas0601_tables.generated.hpp"
#include "registry/vmti0903_tables.generated.hpp"
#include "registry/vtarget0903_tables.generated.hpp"

namespace misbklv {

// Resolve a nested item's child (ADR 0010 childRegistry routing).
const Registry* registry_for(RegistryId id);

// Select a registry by a standalone packet's 16-byte UL key (demux dispatch):
// how the standalone-VMTI vs 0601 variants are told apart on the wire.
const Registry* registry_by_key(std::span<const std::byte> key);

}  // namespace misbklv
