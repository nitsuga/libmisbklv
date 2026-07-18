// SPDX-License-Identifier: Apache-2.0
// Registry resolver: maps a RegistryId (a descriptor's `child`) to its table,
// so the recursive core can descend into nested Local Sets (ADR 0010).
#pragma once

#include "misbklv/types.hpp"
#include "registry/uas0601_tables.generated.hpp"
#include "registry/vmti0903_tables.generated.hpp"

namespace misbklv {

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

}  // namespace misbklv
