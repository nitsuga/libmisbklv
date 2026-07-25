// SPDX-License-Identifier: Apache-2.0
// Decode ST 0601.19's own worked examples through the registry.
//
// Every §8.N item block ends with an "Example Software Value" / "Example KLV
// Item (All Hex)" pair. Those pairs are the standard's own statement of the
// tag's scale, so running them is what proves a descriptor's min/max/length
// right — the stream round-trip tests only prove encode and decode are mutual
// inverses, which a wrongly-scaled mapping also satisfies.
//
// Tolerance is one quantization step of the item (its documented Resolution).
// The spec prints its example software values rounded, so a couple of them sit
// a fraction of an LSB off the exact mapping (tags 26 and 83); demanding better
// than the item's own resolution would be testing the PDF's rounding, not us.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "misbklv/codec.hpp"
#include "misbklv/registries.hpp"
#include "misbklv/types.hpp"

using namespace misbklv;

static int failures = 0;

static std::vector<std::byte> unhex(const std::string& s) {
  std::vector<std::byte> out;
  for (std::size_t i = 0; i + 1 < s.size(); i += 2)
    out.push_back(static_cast<std::byte>(std::stoi(s.substr(i, 2), nullptr, 16)));
  return out;
}

// Decode `hex` as tag `tag` and compare to the spec's example value.
static void ck_real(std::uint16_t tag, const char* hex, double want, double lsb) {
  const ItemDescriptor* d = gen::uas_0601.find(tag);
  if (!d) {
    std::printf("  tag %-3u  NOT REGISTERED\n", tag);
    ++failures;
    return;
  }
  const std::vector<std::byte> raw = unhex(hex);
  auto v = codec::decode(*d, raw);
  const double got = std::get<double>(*v);
  const bool ok = v && std::fabs(got - want) <= lsb;
  std::printf("  tag %-3u %-9s -> %18.9f  (spec %.9f, tol %g)  %s  %s\n", tag, hex,
              got, want, lsb, ok ? "OK" : "FAIL", d->name.data());
  if (!ok) ++failures;
}

static void ck_uint(std::uint16_t tag, const char* hex, std::uint64_t want) {
  const ItemDescriptor* d = gen::uas_0601.find(tag);
  const std::vector<std::byte> raw = unhex(hex);
  auto v = codec::decode(*d, raw);
  const auto got = std::get<std::uint64_t>(*v);
  const bool ok = v && got == want;
  std::printf("  tag %-3u %-9s -> %18llu  (spec %llu)  %s  %s\n", tag, hex,
              static_cast<unsigned long long>(got),
              static_cast<unsigned long long>(want), ok ? "OK" : "FAIL", d->name.data());
  if (!ok) ++failures;
}

static void ck_int(std::uint16_t tag, const char* hex, std::int64_t want) {
  const ItemDescriptor* d = gen::uas_0601.find(tag);
  const std::vector<std::byte> raw = unhex(hex);
  auto v = codec::decode(*d, raw);
  const auto got = std::get<std::int64_t>(*v);
  const bool ok = v && got == want;
  std::printf("  tag %-3u %-9s -> %18lld  (spec %lld)  %s  %s\n", tag, hex,
              static_cast<long long>(got), static_cast<long long>(want),
              ok ? "OK" : "FAIL", d->name.data());
  if (!ok) ++failures;
}

int main() {
  // One vector per distinct (kind, range, width) the registry introduces, so a
  // transcription slip in any mapping family shows up here.
  std::printf("linear LDS — unsigned, offset and plain:\n");
  ck_real(35, "A7C4", 235.924010, 360.0 / 65535);        // 0..360 over uint16
  ck_real(37, "BEBA", 3725.18502, 5000.0 / 65535);       // 0..5000 mbar
  ck_real(43, "03", 6.0, 510.0 / 255);                   // 0..510 px over uint8
  ck_real(45, "1A95", 425.215152, 4095.0 / 65535);       // 0..4095 m
  ck_real(55, "81", 50.5882353, 100.0 / 255);            // 0..100 % over uint8
  ck_real(58, "A45D", 6420.53864, 10000.0 / 65535);      // 0..10000 kg

  std::printf("linear LDS — signed (symmetric about zero):\n");
  ck_real(26, "1750", 0.0136602540, 0.075 / 32767);      // +/-0.075 over int16
  ck_real(51, "D3FE", -61.8878750, 180.0 / 32767);       // +/-180 m/s
  ck_real(79, "09FB", 25.4977569, 327.0 / 32767);        // +/-327 m/s
  ck_real(83, "14BCB2C0", 29.161550376960857,            // +/-180 over int32
          180.0 / 2147483647);
  ck_real(90, "FF62E2F2", -0.43152510208614414,          // +/-90 over int32
          90.0 / 2147483647);
  ck_real(93, "DE179323", -47.683, 180.0 / 2147483647);

  // IMAPB items are variable-length in 0601: each vector below is the length
  // the spec's own example uses, which is not always the registry's default
  // encode width — decoding is driven by the actual field length.
  std::printf("IMAPB (ST 1201):\n");
  ck_real(96, "00D92A", 13898.5463, 0.25);               // (0, 1500000), 3 B
  ck_real(103, "2F921E", 23456.24, 0.078125);            // (-900, 40000), 3 B
  ck_real(109, "0001A0", 1.625, 0.0039);                 // (0, 21000) km, 3 B
  ck_real(112, "1F40", 125.0, 0.016625);                 // (0, 360), 2 B
  ck_real(117, "3E90", 1.0, 0.0625);                     // (-1000, 1000) dps, 2 B
  ck_real(120, "4800", 72.0, 0.004);                     // (0, 100) %, 2 B
  ck_real(132, "0257C0", 2400.0, 0.015625);              // (1, 99999) MHz, 3 B
  ck_real(134, "3700", 55.0, 0.0039);                    // (0, 100) %, 2 B

  std::printf("integers (variable width — decoded at the field's own length):\n");
  ck_uint(110, "4DAF", 19887);                           // uint, 2 of max 4 B
  ck_int(39, "54", 84);                                  // int8, direct
  ck_int(136, "1E", 30);                                 // int, 1 of max 4 B

  std::printf("%s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
