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

static std::vector<std::byte> make_packet() {
  const std::byte payload[] = {std::byte{0xAB}, std::byte{0xCD}};
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
  for (int i = 0; i < 100; ++i) {
    auto e = framer.feed(bad, i, [&](const KlvPacket&) { ++packets; });
    check(e && *e == Error::BadLength, "junk feed reports BadLength") ;
    if (failures) break;
  }
  check(framer.remainder().size() <= before + bad.size(), "buffer does not grow on repeated junk");
  check(packets == 2, "junk emits no packets");

  std::printf("%s\n", failures == 0 ? "KLV_FRAMER: all PASS" : "KLV_FRAMER: FAIL");
  return failures == 0 ? 0 : 1;
}
