// SPDX-License-Identifier: Apache-2.0
// LocalSetBuilder::check_mandatory() (ADR 0040), exercised for the CompositeImaging1602
// registry (ADR 0041): the checksum-free mandatory check a caller authoring a nested
// Composite Imaging LS (ST 0601 Item 99) uses before serialize_items().
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
  const Registry& reg = gen::composite_imaging_1602;

  {
    // ST 0601 Item 99 routes to CompositeImaging1602 (ADR 0041): the routing
    // registry_for() depends on, not just the registry existing in isolation.
    const ItemDescriptor* d99 = gen::uas_0601.find(99);
    check(d99 != nullptr, "uas0601 has Item 99");
    check(d99 && d99->kind == ValueKind::NestedLS, "Item 99 is NestedLS");
    check(d99 && registry_for(d99->child) == &reg,
          "Item 99's child resolves to CompositeImaging1602");
  }

  {
    // (b) tag 18 (Z-Order) omitted -> MissingMandatory.
    LocalSetBuilder b(reg);
    check(bool(b.set(2, std::uint64_t{0x01})), "stage tag 2");   // Document Version
    check(bool(b.set(9, std::uint64_t{480})), "stage tag 9");    // Sub-Image Rows
    check(bool(b.set(10, std::uint64_t{640})), "stage tag 10");  // Sub-Image Columns
    check(bool(b.set(11, std::int64_t{0})), "stage tag 11");     // Sub-Image Position X
    check(bool(b.set(12, std::int64_t{0})), "stage tag 12");     // Sub-Image Position Y
    auto r = b.check_mandatory();
    check(!r && r.error() == Error::MissingMandatory, "tag 18 omitted -> MissingMandatory");
  }

  {
    // (c) all six mandatory tags staged -> ok.
    LocalSetBuilder b(reg);
    check(bool(b.set(2, std::uint64_t{0x01})), "stage tag 2");
    check(bool(b.set(9, std::uint64_t{480})), "stage tag 9");
    check(bool(b.set(10, std::uint64_t{640})), "stage tag 10");
    check(bool(b.set(11, std::int64_t{0})), "stage tag 11");
    check(bool(b.set(12, std::int64_t{0})), "stage tag 12");
    check(bool(b.set(18, std::uint64_t{1})), "stage tag 18");  // Z-Order
    auto r = b.check_mandatory();
    check(bool(r), "all six mandatory tags staged -> ok");
  }

  return failures ? 1 : 0;
}
