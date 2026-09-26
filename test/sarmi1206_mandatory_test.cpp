// SPDX-License-Identifier: Apache-2.0
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "misbklv/ber.hpp"
#include "misbklv/builder.hpp"
#include "misbklv/mdarray.hpp"
#include "misbklv/registries.hpp"

using namespace misbklv;

static int failures = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("  %-42s FAIL\n", what);
    ++failures;
  }
}

static void append_float(ber::Bytes& out, float value) {
  std::uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  out.push_back(static_cast<std::byte>(bits >> 24));
  out.push_back(static_cast<std::byte>(bits >> 16));
  out.push_back(static_cast<std::byte>(bits >> 8));
  out.push_back(static_cast<std::byte>(bits));
}

static float read_float(std::span<const std::byte> bytes) {
  const auto bits = (std::to_integer<std::uint32_t>(bytes[0]) << 24) |
                    (std::to_integer<std::uint32_t>(bytes[1]) << 16) |
                    (std::to_integer<std::uint32_t>(bytes[2]) << 8) |
                    std::to_integer<std::uint32_t>(bytes[3]);
  return std::bit_cast<float>(bits);
}

int main() {
  const Registry& reg = gen::sarmi_1206;
  const ItemDescriptor* d95 = gen::uas_0601.find(95);
  const ItemDescriptor* d1 = reg.find(1);
  const ItemDescriptor* d2 = reg.find(2);
  const ItemDescriptor* d3 = reg.find(3);
  const ItemDescriptor* d22 = reg.find(22);
  const ItemDescriptor* d28 = reg.find(28);
  check(reg.items.size() == 28, "SARMI has all Table 1 tags");
  check(d95 && d95->kind == ValueKind::NestedLS, "Item 95 is NestedLS");
  check(d95 && registry_for(d95->child) == &reg, "Item 95 resolves to Sarmi1206");
  check(d1 && d1->kind == ValueKind::IMAPB && d1->fixed_len == 2 && d1->map.min == 0.0 &&
            d1->map.max == 90.0,
        "Tag 1 IMAPB descriptor");
  check(d2 && d2->flags == kMandatory && d3 && d3->flags == kMandatory,
        "Tags 2 and 3 mandatory descriptors");
  check(d22 && d22->kind == ValueKind::MdArray, "Tag 22 is MdArray");
  check(d28 && d28->kind == ValueKind::UInt && d28->fixed_len == 1 && d28->flags == kMandatory,
        "Tag 28 mandatory descriptor");

  LocalSetBuilder missing(reg);
  check(bool(missing.set(2, 0.0)), "stage tag 2");
  check(bool(missing.set(3, std::uint64_t{1})), "stage tag 3");
  auto incomplete = missing.check_mandatory();
  check(!incomplete && incomplete.error() == Error::MissingMandatory,
        "missing document version -> MissingMandatory");

  LocalSetBuilder missing_squint(reg);
  check(bool(missing_squint.set(3, std::uint64_t{1})), "stage missing-squint tag 3");
  check(bool(missing_squint.set(28, std::uint64_t{1})), "stage missing-squint tag 28");
  auto missing_squint_result = missing_squint.check_mandatory();
  check(!missing_squint_result && missing_squint_result.error() == Error::MissingMandatory,
        "missing squint -> MissingMandatory");

  LocalSetBuilder missing_direction(reg);
  check(bool(missing_direction.set(2, 0.0)), "stage missing-direction tag 2");
  check(bool(missing_direction.set(28, std::uint64_t{1})), "stage missing-direction tag 28");
  auto missing_direction_result = missing_direction.check_mandatory();
  check(!missing_direction_result && missing_direction_result.error() == Error::MissingMandatory,
        "missing look direction -> MissingMandatory");

  LocalSetBuilder complete(reg);
  check(bool(complete.set(2, 0.0)), "stage complete tag 2");
  check(bool(complete.set(3, std::uint64_t{1})), "stage complete tag 3");
  check(bool(complete.set(28, std::uint64_t{1})), "stage document version");
  check(bool(complete.check_mandatory()), "all mandatory tags -> ok");

  ber::Bytes raw_mdarray;
  ber::write_oid(raw_mdarray, 2);  // two dimensions
  ber::write_oid(raw_mdarray, 1);
  ber::write_oid(raw_mdarray, 1);
  ber::write_oid(raw_mdarray, 2);
  ber::write_oid(raw_mdarray, static_cast<std::uint8_t>(mdarray::Apa::Imapb));
  append_float(raw_mdarray, 0.0f);
  append_float(raw_mdarray, 1000000.0f);
  raw_mdarray.push_back(std::byte{0});
  raw_mdarray.push_back(std::byte{0});
  auto parsed = mdarray::parse(raw_mdarray);
  auto coefficient = parsed ? parsed->element_imapb(0) : Result<double>::err(Error::BadLength);
  check(parsed && parsed->dims.size() == 2 && parsed->apa == mdarray::Apa::Imapb,
        "Tag 22 MDARRAY shape and APA");
  check(parsed && parsed->apas.size() == 8 &&
            std::fabs(read_float(parsed->apas.subspan(4)) - 1000000.0f) < 1.0f,
        "Tag 22 IMAPB upper bound");
  check(coefficient && std::fabs(*coefficient) < 1e-6, "Tag 22 IMAPB decode");
  return failures ? 1 : 0;
}
