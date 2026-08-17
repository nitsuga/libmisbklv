// SPDX-License-Identifier: Apache-2.0
// Milestone 4: ST 1201 IMAPB codec, validated against the standard's own worked
// example (ST 1201.5 §10 Example 3 / Table 7: IMAPB(-900, 19000), L=3).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
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

static void expect_pred(const char* what, bool ok, double got) {
  std::printf("  %-28s = %g -> %s\n", what, got, ok ? "OK" : "FAIL");
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

  // cross-check vs ST 1201 Annex A test vectors (transcribed by jmisb,
  // WestRidgeSystems, MIT) — independent confirmation, incl. the non-zero
  // Zoffset path (a<0<b) that regressed in M6.
  std::printf("ST 1201 Annex A cross-check (via jmisb):\n");
  const ItemDescriptor a0 = imapb(0.0, 100.0);   // Zoffset = 0
  expect_bytes("IMAPB(0,100,3) enc(10.1)", enc(a0, 10.1, 3), {0x0A, 0x19, 0x99});
  expect_bytes("IMAPB(0,100,3) enc(50.5)", enc(a0, 50.5, 3), {0x32, 0x80, 0x00});
  expect_bytes("IMAPB(0,100,3) enc(100.0)", enc(a0, 100.0, 3), {0x64, 0x00, 0x00});
  const ItemDescriptor az = imapb(-9.9, 110.0);  // Zoffset != 0 (a<0<b)
  expect_bytes("IMAPB(-9.9,110,3) enc(0.0)", enc(az, 0.0, 3), {0x09, 0xE6, 0x67});
  expect_bytes("IMAPB(-9.9,110,3) enc(30.6)", enc(az, 30.6, 3), {0x28, 0x80, 0x00});
  expect_bytes("IMAPB(-9.9,110,3) enc(110)", enc(az, 110.0, 3), {0x77, 0xE6, 0x67});
  const ItemDescriptor as = imapb(0.1, 0.9);
  expect_bytes("IMAPB(0.1,0.9,2) enc(0.5)", enc(as, 0.5, 2), {0x33, 0x33});
  expect_bytes("IMAPB(0.1,0.9,2) enc(0.9)", enc(as, 0.9, 2), {0x66, 0x66});

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

  // IMAP structural special values (ST 1201 §7.2.3 Tables 2-3); jmisb
  // IMAPB(0,100,3) vectors. Encode signals; decode -> IEEE / clamped-to-range.
  const ItemDescriptor sp = imapb(0.0, 100.0);
  const double kInf = std::numeric_limits<double>::infinity();
  const double kNaN = std::numeric_limits<double>::quiet_NaN();
  std::printf("IMAP special values (ST 1201 Tables 2-3):\n");
  expect_bytes("enc(+inf)", enc(sp, kInf, 3), {0xC8, 0x00, 0x00});
  expect_bytes("enc(-inf)", enc(sp, -kInf, 3), {0xE8, 0x00, 0x00});
  expect_bytes("enc(NaN)", enc(sp, kNaN, 3), {0xD0, 0x00, 0x00});
  expect_bytes("enc(-1.0 below min)", enc(sp, -1.0, 3), {0xE0, 0x00, 0x00});
  expect_bytes("enc(101.0 above max)", enc(sp, 101.0, 3), {0xE1, 0x00, 0x00});
  auto dsp = [&](std::initializer_list<int> bytes) {
    ber::Bytes b;
    for (int v : bytes) b.push_back(static_cast<std::byte>(v));
    return codec::imapb_decode(sp, b);
  };
  expect_pred("dec(0xC80000)=+inf", std::isinf(dsp({0xC8, 0, 0})) && dsp({0xC8, 0, 0}) > 0,
              dsp({0xC8, 0, 0}));
  expect_pred("dec(0xE80000)=-inf", std::isinf(dsp({0xE8, 0, 0})) && dsp({0xE8, 0, 0}) < 0,
              dsp({0xE8, 0, 0}));
  expect_pred("dec(0xD00000)=NaN", std::isnan(dsp({0xD0, 0, 0})), 0.0);
  expect_near("dec(0xE00000)->min", dsp({0xE0, 0, 0}), 0.0);    // below -> min
  expect_near("dec(0xE10000)->max", dsp({0xE1, 0, 0}), 100.0);  // above -> max

  // Degenerate descriptors (issue #7 item 1): a caller-supplied min >= max has
  // no valid IMAPB scale — log2(b - a) is non-finite, so the unguarded float->int
  // cast was UB (caught under -fsanitize=undefined). Encode now emits the +QNaN
  // special and decode returns NaN; nothing crashes or wraps.
  std::printf("degenerate descriptor guards (issue #7):\n");
  const ItemDescriptor degen = imapb(5.0, 5.0);  // min == max
  expect_bytes("enc(min==max) -> +QNaN", enc(degen, 5.0, 3), {0xD0, 0x00, 0x00});
  expect_pred("dec(min==max) -> NaN",
              std::isnan(codec::imapb_decode(degen, enc(degen, 5.0, 3))), 0.0);
  const ItemDescriptor inv = imapb(10.0, 5.0);   // min > max
  expect_pred("enc(min>max) -> special, no UB",
              codec::is_imap_special(enc(inv, 7.0, 3)) && enc(inv, 7.0, 3).size() == 3, 0.0);

  // Over-long field (issue #7 item 2): a direct caller could hand imapb_decode
  // more than 8 bytes. It must clamp to the low 8 rather than let a bogus width
  // reach imapb_params; the result is out of contract but must be finite, never
  // UB. (The exact low-8 read is pinned on rd_uint in hardening_test.)
  {
    const ItemDescriptor a0 = imapb(0.0, 100.0);
    ber::Bytes wide(9, std::byte{0});
    wide[0] = std::byte{0xAA};  // 9-byte field: leading byte dropped by the clamp
    expect_pred("dec(9 bytes) stays finite, no UB",
                std::isfinite(codec::imapb_decode(a0, wide)), 0.0);
  }

  std::printf("IMAPB: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
