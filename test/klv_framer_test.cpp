// SPDX-License-Identifier: Apache-2.0
// KlvFramer resync (issue #80 F2): a malformed frame (indefinite BER 0x80) must
// be skipped, not re-parsed forever; later valid packets still emit and the bad
// bytes are not retained.
#include <cstdio>
#include <cstdint>
#include <span>
#include <vector>

#include "../src/klv_framer.hpp"
#include "misbklv/builder.hpp"
#include "misbklv/packet.hpp"
#include "misbklv/registry/uas0601_tables.generated.hpp"

using namespace misbklv;
using namespace misbklv::detail;

static int failures = 0;
static void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}

static std::vector<std::byte> make_packet(std::size_t payload_bytes = 2) {
  const std::vector<std::byte> payload(payload_bytes, std::byte{0xAB});
  LocalSetBuilder b(gen::uas_0601);
  b.append_raw(143, payload);
  auto pkt = std::move(b).finalize(std::span{kUas0601Key}, /*enforce=*/false);
  return pkt ? *pkt : std::vector<std::byte>{};
}

// UL prefix + 12 zero bytes + indefinite-length BER 0x80.
static std::vector<std::byte> bad_frame() {
  std::vector<std::byte> v = {std::byte{0x06}, std::byte{0x0e}, std::byte{0x2b}, std::byte{0x34}};
  v.insert(v.end(), 12, std::byte{0});
  v.push_back(std::byte{0x80});
  return v;
}

int main() {
  const auto good = make_packet();
  check(!good.empty(), "build valid packet");

  std::vector<std::byte> stream = good;
  const auto bad = bad_frame();
  stream.insert(stream.end(), bad.begin(), bad.end());
  stream.insert(stream.end(), good.begin(), good.end());

  KlvFramer framer;
  int packets = 0;
  auto err = framer.feed(stream, 0, [&](const KlvPacket&) { ++packets; });
  check(packets == 2, "both valid packets emitted around the malformed frame");
  check(err && *err == Error::BadLength, "first error (BadLength) reported");
  check(framer.remainder().size() < bad.size(), "bad bytes not retained in remainder");

  const std::size_t before = framer.remainder().size();
  bool junk_ok = true;
  for (int i = 0; i < 100; ++i) {
    auto e = framer.feed(bad, i, [&](const KlvPacket&) { ++packets; });
    if (!(e && *e == Error::BadLength)) {
      junk_ok = false;
      break;
    }
  }
  check(junk_ok, "100 junk feeds each report BadLength");
  check(framer.remainder().size() <= before + bad.size(), "buffer does not grow on repeated junk");
  check(packets == 2, "junk emits no packets");

  // Over-cap: a valid-but-too-large packet is ResourceLimit, skipped without
  // wedging; the following packet that fits the cap still emits.
  {
    const auto big = make_packet(40);
    check(big.size() > good.size(), "oversize packet is larger than the normal one");
    std::vector<std::byte> s2 = big;
    s2.insert(s2.end(), good.begin(), good.end());
    KlvFramer capped(good.size());
    int n = 0;
    auto e = capped.feed(s2, 0, [&](const KlvPacket&) { ++n; });
    check(e && *e == Error::ResourceLimit, "over-cap packet reports ResourceLimit");
    check(n == 1, "packet under the cap still emitted after over-cap packet");
    check(capped.remainder().empty(), "oversize UL not retained (framer not wedged)");
  }

  // Next feed completes a packet split across feeds.
  {
    const std::span<const std::byte> all(good);
    const auto half = all.subspan(0, good.size() / 2);
    const auto rest = all.subspan(good.size() / 2);
    KlvFramer f;
    int n = 0;
    auto e1 = f.feed(half, 0, [&](const KlvPacket&) { ++n; });
    check(!e1 && n == 0, "half packet: no error, nothing emitted");
    check(f.remainder().size() == half.size(), "half packet stays buffered");
    auto e2 = f.feed(rest, 1, [&](const KlvPacket&) { ++n; });
    check(!e2 && n == 1, "rest completes exactly one packet");
    check(f.remainder().empty(), "remainder empty after completion");
  }

  // Same, after a bad frame in the first feed: resync and reassembly compose.
  {
    const std::span<const std::byte> all(good);
    const auto half = all.subspan(0, good.size() / 2);
    const auto rest = all.subspan(good.size() / 2);
    std::vector<std::byte> first = bad;
    first.insert(first.end(), half.begin(), half.end());
    KlvFramer f;
    int n = 0;
    auto e1 = f.feed(first, 0, [&](const KlvPacket&) { ++n; });
    check(e1 && *e1 == Error::BadLength && n == 0,
          "bad frame then half packet: BadLength, nothing emitted");
    check(f.remainder().size() == half.size(), "only the half packet stays buffered");
    auto e2 = f.feed(rest, 1, [&](const KlvPacket&) { ++n; });
    check(!e2 && n == 1, "rest completes exactly one packet after resync");
    check(f.remainder().empty(), "remainder empty after completion");
  }

  std::printf("%s\n", failures == 0 ? "KLV_FRAMER: all PASS" : "KLV_FRAMER: FAIL");
  return failures == 0 ? 0 : 1;
}
