// SPDX-License-Identifier: Apache-2.0
// Cross-check our registered ST 0601 + ST 0903 codecs against
// WestRidgeSystems/jmisb test vectors (MIT). Matching jmisb's bytes is
// independent confirmation of our linear-LDS map (which the ST 1201 Annex A
// vectors, being IMAPB-only, did not cover), of the KB claim that 0601
// lat/lon/angles are legacy linear (NOT ST 1201 IMAP), and of our 0903 VTarget
// item ranges (offsetLat IMAPB(-19.2,19.2,3); targetColor uint24 RGB).
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

static void ck_enc(const Registry& reg, std::uint16_t tag, double x,
                   std::initializer_list<int> want, const char* label) {
  const ItemDescriptor* d = reg.find(tag);
  auto enc = codec::encode(*d, Value{x}, want.size());
  bool ok = enc && enc->size() == want.size();
  std::size_t i = 0;
  for (int w : want)
    if (ok && std::to_integer<int>((*enc)[i++]) != w) ok = false;
  std::printf("  %-22s enc(%.10g) -> ", label, x);
  if (enc)
    for (auto b : *enc) std::printf("%02X", std::to_integer<unsigned>(b));
  std::printf("  %s\n", ok ? "OK" : "FAIL");
  if (!ok) ++failures;
}

static void ck_enc_u(const Registry& reg, std::uint16_t tag, std::uint64_t x,
                     std::initializer_list<int> want, const char* label) {
  const ItemDescriptor* d = reg.find(tag);
  auto enc = codec::encode(*d, Value{x}, want.size());
  bool ok = enc && enc->size() == want.size();
  std::size_t i = 0;
  for (int w : want)
    if (ok && std::to_integer<int>((*enc)[i++]) != w) ok = false;
  std::printf("  %-22s enc(0x%llX) -> ", label, static_cast<unsigned long long>(x));
  if (enc)
    for (auto b : *enc) std::printf("%02X", std::to_integer<unsigned>(b));
  std::printf("  %s\n", ok ? "OK" : "FAIL");
  if (!ok) ++failures;
}

static void ck_dec(const Registry& reg, std::uint16_t tag, std::vector<int> bytes, double want,
                   const char* label) {
  const ItemDescriptor* d = reg.find(tag);
  ber::Bytes raw;
  for (int b : bytes) raw.push_back(static_cast<std::byte>(b));
  auto v = codec::decode(*d, raw);
  double got = std::get<double>(*v);
  const bool ok = std::fabs(got - want) < 1e-6;
  std::printf("  %-22s dec -> %.10f (want %.10f)  %s\n", label, got, want, ok ? "OK" : "FAIL");
  if (!ok) ++failures;
}

int main() {
  const Registry& r = gen::uas_0601;

  std::printf("ST 0601 Sensor Latitude (tag 13, linear int32 +/-90):\n");
  ck_enc(r, 13, -90.0, {0x80, 0x00, 0x00, 0x01}, "lat -90");
  ck_enc(r, 13, 90.0, {0x7F, 0xFF, 0xFF, 0xFF}, "lat 90");
  ck_enc(r, 13, 0.0, {0x00, 0x00, 0x00, 0x00}, "lat 0");
  ck_enc(r, 13, 60.1768229669783, {0x55, 0x95, 0xB6, 0x6D}, "lat 60.176...");
  ck_dec(r, 13, {0x55, 0x95, 0xB6, 0x6D}, 60.1768229669783, "lat 60.176...");
  // NB: 0x80000000 is a special (jmisb: +inf; MISB: "Reserved") — our codec
  // doesn't yet model linear/IMAP structural specials (see PROGRESS Known gaps).

  std::printf("ST 0601 Sensor True Altitude (tag 15, linear uint16 -900..19000):\n");
  ck_enc(r, 15, -900.0, {0x00, 0x00}, "alt -900");
  ck_enc(r, 15, 19000.0, {0xFF, 0xFF}, "alt 19000");
  ck_enc(r, 15, 14190.72, {0xC2, 0x21}, "alt 14190.72");

  std::printf("ST 0601 Platform Heading Angle (tag 5, linear uint16 0..360):\n");
  ck_enc(r, 5, 0.0, {0x00, 0x00}, "hdg 0");
  ck_enc(r, 5, 360.0, {0xFF, 0xFF}, "hdg 360");
  ck_enc(r, 5, 159.9744, {0x71, 0xC2}, "hdg 159.9744");

  const Registry& vt = gen::vtarget_0903;
  std::printf("ST 0903 VTarget targetLocationOffsetLat (tag 10, IMAPB(-19.2,19.2,3)):\n");
  ck_enc(vt, 10, 10.0, {0x3A, 0x66, 0x67}, "offsetLat 10.0");
  ck_dec(vt, 10, {0x3A, 0x66, 0x67}, 10.0, "offsetLat 10.0");
  std::printf("ST 0903 VTarget targetColor (tag 8, uint24 RGB 85,136,51):\n");
  ck_enc_u(vt, 8, 0x558833, {0x55, 0x88, 0x33}, "targetColor");

  std::printf("jmisb ST 0601/0903 cross-check: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
