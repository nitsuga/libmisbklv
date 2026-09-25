// SPDX-License-Identifier: Apache-2.0
// LocalSetBuilder::check_mandatory() (ADR 0040): the checksum-free mandatory
// check a caller authoring a nested Security0102 LS (ST 0601 Item 48) uses
// before serialize_items(), since finalize()'s own checksum tag collides with
// Security0102's own tag 1 (Security Classification).
#include <cstdio>

#include "misbklv/builder.hpp"
#include "misbklv/registries.hpp"
#include "misbklv/types.hpp"

using namespace misbklv;

static int failures = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("  %-42s FAIL\n", what);
    ++failures;
  }
}

int main() {
  const Registry& reg = gen::security_0102;

  {
    // ST 0601 Item 48 routes to Security0102 (ADR 0040): the routing
    // registry_for() depends on, not just the registry existing in isolation.
    const ItemDescriptor* d48 = gen::uas_0601.find(48);
    check(d48 != nullptr, "uas0601 has Item 48");
    check(d48 && d48->kind == ValueKind::NestedLS, "Item 48 is NestedLS");
    check(d48 && registry_for(d48->child) == &reg, "Item 48's child resolves to Security0102");
  }

  {
    // (a) tag 22 (Version) omitted -> MissingMandatory.
    LocalSetBuilder b(reg);
    const std::uint8_t country[] = {'U', 'S'};
    check(bool(b.set(1, std::uint64_t{0x01})), "stage tag 1");     // Security Classification
    check(bool(b.set(2, std::uint64_t{0x01})), "stage tag 2");     // Country Coding Method
    check(bool(b.set(3, std::string_view{"US"})), "stage tag 3");  // Classifying Country
    check(bool(b.set(12, std::uint64_t{0x01})), "stage tag 12");   // Object Country Coding Method
    check(
        bool(b.set(13, std::span<const std::byte>(reinterpret_cast<const std::byte*>(country), 2))),
        "stage tag 13");  // Object Country Codes
    auto r = b.check_mandatory();
    check(!r && r.error() == Error::MissingMandatory, "tag 22 omitted -> MissingMandatory");
  }

  {
    // (b) all six mandatory tags staged -> ok.
    LocalSetBuilder b(reg);
    const std::uint8_t country[] = {'U', 'S'};
    check(bool(b.set(1, std::uint64_t{0x01})), "stage tag 1");
    check(bool(b.set(2, std::uint64_t{0x01})), "stage tag 2");
    check(bool(b.set(3, std::string_view{"US"})), "stage tag 3");
    check(bool(b.set(12, std::uint64_t{0x01})), "stage tag 12");
    check(
        bool(b.set(13, std::span<const std::byte>(reinterpret_cast<const std::byte*>(country), 2))),
        "stage tag 13");
    check(bool(b.set(22, std::uint64_t{0x000A})), "stage tag 22");
    auto r = b.check_mandatory();
    check(bool(r), "all six mandatory tags staged -> ok");
  }

  return failures ? 1 : 0;
}
