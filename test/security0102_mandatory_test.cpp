// SPDX-License-Identifier: Apache-2.0
// LocalSetBuilder::check_mandatory() (ADR 0040): the checksum-free mandatory
// check a caller authoring a nested Security0102 LS (ST 0601 Item 48) uses
// before serialize_items(), since finalize()'s own checksum tag collides with
// Security0102's own tag 1 (Security Classification).
#include <cstdio>
#include <cstring>

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
    // (a) tag 22 (Version) omitted -> MissingMandatory.
    LocalSetBuilder b(reg);
    const std::uint8_t country[] = {'U', 'S'};
    (void)b.set(1, std::uint64_t{0x01});                                    // Security Classification
    (void)b.set(2, std::uint64_t{0x01});                                    // Country Coding Method
    (void)b.set(3, std::string_view{"US"});                                 // Classifying Country
    (void)b.set(12, std::uint64_t{0x01});                                   // Object Country Coding Method
    (void)b.set(13, std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(country), 2));   // Object Country Codes
    auto r = b.check_mandatory();
    check(!r && r.error() == Error::MissingMandatory,
          "tag 22 omitted -> MissingMandatory");
  }

  {
    // (b) all six mandatory tags staged -> ok.
    LocalSetBuilder b(reg);
    const std::uint8_t country[] = {'U', 'S'};
    (void)b.set(1, std::uint64_t{0x01});
    (void)b.set(2, std::uint64_t{0x01});
    (void)b.set(3, std::string_view{"US"});
    (void)b.set(12, std::uint64_t{0x01});
    (void)b.set(13, std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(country), 2));
    (void)b.set(22, std::uint64_t{0x000A});
    auto r = b.check_mandatory();
    check(bool(r), "all six mandatory tags staged -> ok");
  }

  return failures ? 1 : 0;
}
