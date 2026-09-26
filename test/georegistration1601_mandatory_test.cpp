// SPDX-License-Identifier: Apache-2.0
#include <cstdio>

#include "misbklv/builder.hpp"
#include "misbklv/registries.hpp"

using namespace misbklv;

static int failures = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("  %-42s FAIL\n", what);
    ++failures;
  }
}

int main() {
  const Registry& reg = gen::geo_registration_1601;
  const ItemDescriptor* d98 = gen::uas_0601.find(98);
  check(d98 && d98->kind == ValueKind::NestedLS, "Item 98 is NestedLS");
  check(d98 && registry_for(d98->child) == &reg, "Item 98 resolves to GeoRegistration1601");

  LocalSetBuilder missing(reg);
  check(bool(missing.set(1, std::uint64_t{2})), "stage document version");
  check(bool(missing.set(2, std::string_view{"geo"})), "stage algorithm name");
  auto incomplete = missing.check_mandatory();
  check(!incomplete && incomplete.error() == Error::MissingMandatory,
        "missing algorithm version -> MissingMandatory");

  LocalSetBuilder complete(reg);
  check(bool(complete.set(1, std::uint64_t{2})), "stage complete document version");
  check(bool(complete.set(2, std::string_view{"geo"})), "stage complete algorithm name");
  check(bool(complete.set(3, std::string_view{"1"})), "stage complete algorithm version");
  check(bool(complete.check_mandatory()), "all mandatory tags staged -> ok");
  return failures ? 1 : 0;
}
