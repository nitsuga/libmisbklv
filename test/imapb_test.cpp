// SPDX-License-Identifier: Apache-2.0
// Milestone 4: ST 1201 IMAPB codec, validated against the standard's own worked
// example (ST 1201.5 §10 Example 3 / Table 7: IMAPB(-900, 19000), L=3).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "misbklv/codec.hpp"
#include "misbklv/types.hpp"

using namespace misbklv;

static int failures = 0;

static ItemDescriptor imapb(double a, double b) {
  ItemDescriptor d{};
  d.kind = ValueKind::IMAPB;
  d.map = {a, b};
  return d;
}

static ber::Bytes enc(const ItemDescriptor& d, double x, int len) {
  ber::Bytes out;
  codec::imapb_encode(d, x, out, len);
  return out;
}

static void expect_bytes(const char* what, const ber::Bytes& got,
                         std::initializer_list<int> want) {
  bool ok = got.size() == want.size();
  std::size_t i = 0;
  for (int w : want)
    if (ok && std::to_integer<int>(got[i++]) != w) ok = false;
  std::printf("  %-28s ", what);
  for (auto b : got) std::printf("%02X ", std::to_integer<unsigned>(b));
  std::printf("-> %s\n", ok ? "OK" : "FAIL");
  if (!ok) ++failures;
}

static void expect_near(const char* what, double got, double want) {
  const bool ok = std::fabs(got - want) < 1e-6;
  std::printf("  %-28s = %.6f  (want %.6f) -> %s\n", what, got, want,
              ok ? "OK" : "FAIL");
  if (!ok) ++failures;
}

int main() {
  const ItemDescriptor d = imapb(-900.0, 19000.0);  // ST 1201 Example 3
  std::printf("ST 1201 IMAPB(-900, 19000), L=3 (Table 7):\n");

  // forward: exact integers published in Table 7
  expect_bytes("enc(-900.0)", enc(d, -900.0, 3), {0x00, 0x00, 0x00});
  expect_bytes("enc(0.0)", enc(d, 0.0, 3), {0x03, 0x84, 0x00});     // 230400
  expect_bytes("enc(10.0)", enc(d, 10.0, 3), {0x03, 0x8E, 0x00});   // 232960

  // reverse: exact floats published in Table 7
  auto dec = [&](std::initializer_list<int> bytes) {
    ber::Bytes b;
    for (int v : bytes) b.push_back(static_cast<std::byte>(v));
    return codec::imapb_decode(d, b);
  };
  expect_near("dec(0x038E00)", dec({0x03, 0x8E, 0x00}), 10.0);
  expect_near("dec(0x038400)", dec({0x03, 0x84, 0x00}), 0.0);
  expect_near("dec(0x000000)", dec({0x00, 0x00, 0x00}), -900.0);

  // second standard vector: ST 0903 §10.1.11 — IMAPB(0,180), 12.5° -> 0x0640
  const ItemDescriptor fov = imapb(0.0, 180.0);
  std::printf("ST 0903 IMAPB(0, 180), L=2 (VMTI FoV §10.1.11):\n");
  expect_bytes("enc(12.5)", enc(fov, 12.5, 2), {0x06, 0x40});
  {
    ber::Bytes b{static_cast<std::byte>(0x06), static_cast<std::byte>(0x40)};
    expect_near("dec(0x0640)", codec::imapb_decode(fov, b), 12.5);
  }

  // third vector: ST 0903 §10.2.2.10 VTarget offset — IMAPB(-19.2,19.2) L=3,
  // exercises the non-zero Zoffset path (a<0<b): 10.0° -> 0x3A6667.
  const ItemDescriptor off = imapb(-19.2, 19.2);
  std::printf("ST 0903 IMAPB(-19.2, 19.2), L=3 (VTarget offset §10.2.2.10):\n");
  expect_bytes("enc(10.0)", enc(off, 10.0, 3), {0x3A, 0x66, 0x67});
  {
    ber::Bytes b{static_cast<std::byte>(0x3A), static_cast<std::byte>(0x66),
                 static_cast<std::byte>(0x67)};
    expect_near("dec(0x3A6667)", codec::imapb_decode(off, b), 10.0);
  }

  // round-trip sweep across the range
  std::printf("round-trip sweep:\n");
  bool sweep_ok = true;
  for (double x = -900.0; x <= 19000.0; x += 137.3) {
    double back = codec::imapb_decode(d, enc(d, x, 3));
    if (std::fabs(back - x) > 0.5) {  // L=3 quantization ~ (b-a)/2^23
      sweep_ok = false;
      std::printf("  x=%.3f -> back=%.3f (delta %.3f)\n", x, back, back - x);
    }
  }
  std::printf("  sweep within 1 LSB: %s\n", sweep_ok ? "OK" : "FAIL");
  if (!sweep_ok) ++failures;

  std::printf("IMAPB: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
