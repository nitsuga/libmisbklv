// SPDX-License-Identifier: Apache-2.0
#include "misbklv/registries.hpp"

namespace misbklv {
namespace {
// All top-level registries (those with a 16-byte UL key).
constexpr const Registry* kRegistries[] = {&gen::uas_0601, &gen::vmti_0903};
}  // namespace

const Registry* registry_for(RegistryId id) {
  switch (id) {
    case RegistryId::Uas0601:
      return &gen::uas_0601;
    case RegistryId::Vmti0903:
      return &gen::vmti_0903;
    case RegistryId::Vtarget0903:
      return &gen::vtarget_0903;
    case RegistryId::None:
    default:
      return nullptr;
  }
}

const Registry* registry_by_key(std::span<const std::byte> key) {
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
