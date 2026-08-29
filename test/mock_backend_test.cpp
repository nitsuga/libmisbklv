// SPDX-License-Identifier: Apache-2.0
// MockBackend contract test (ADR 0013) — exercises the extract/insert interface
// with no gstreamer.
#include <cstdio>
#include <span>
#include <vector>

#include "misbklv/mock_backend.hpp"

using namespace misbklv;

static int failures = 0;
static void check(bool ok, const char* what) {
  std::printf("  %-42s %s\n", what, ok ? "OK" : "FAIL");
  if (!ok) ++failures;
}

int main() {
  const ber::Bytes p1{std::byte{1}, std::byte{2}, std::byte{3}};
  const ber::Bytes p2{std::byte{4}, std::byte{5}};

  MockBackend be({p1, p2});
  std::vector<ber::Bytes> got;
  std::vector<std::int64_t> got_pts;
  auto r = be.extract("mock", [&](const KlvPacket& kp) {
    got.emplace_back(kp.bytes.begin(), kp.bytes.end());
    got_pts.push_back(kp.pts_ns);
  });
  check(bool(r), "extract returns ok");
  check(got.size() == 2, "extract yielded 2 packets");
  check(got.size() == 2 && got[0] == p1 && got[1] == p2, "extract bytes match canned");
  check(got_pts == std::vector<std::int64_t>{kNoPts, kNoPts}, "untimed stream extracts as kNoPts");

  // MockBackend applies the byte cap without requiring its canned packets to
  // be structurally valid KLV. An over-limit canned packet is never delivered.
  got.clear();
  auto limited = be.extract(
      "mock", [&](const KlvPacket& kp) { got.emplace_back(kp.bytes.begin(), kp.bytes.end()); }, {},
      ExtractOptions{.max_packet_bytes = 2});
  check(!limited && limited.error() == Error::ResourceLimit && got.empty(),
        "MockBackend limit rejects over-limit canned packet before callback");
  got.clear();
  auto arbitrary = be.extract(
      "mock", [&](const KlvPacket& kp) { got.emplace_back(kp.bytes.begin(), kp.bytes.end()); }, {},
      ExtractOptions{.max_packet_bytes = 3});
  check(arbitrary && got.size() == 2 && got[0] == p1 && got[1] == p2,
        "MockBackend cap preserves within-limit arbitrary canned bytes");

  // ...and a timed one replays its timestamps (ADR 0021): the interface says
  // pts_ns is ns from the start of the source, so the test double must be able
  // to say something other than "no idea".
  MockBackend timed({p1, p2}, {0, 40'000'000});
  got_pts.clear();
  (void)timed.extract("mock", [&](const KlvPacket& kp) { got_pts.push_back(kp.pts_ns); });
  check(got_pts == std::vector<std::int64_t>{0, 40'000'000},
        "timed stream extracts its timestamps");

  auto ins =
      be.open_insert({"file:out.ts", /*realtime=*/false, /*video_source=*/"", Sei0604::Preserve});
  check(bool(ins), "open_insert ok");
  (void)(*ins)->push(std::span<const std::byte>(p1), 42);
  (void)(*ins)->finish();
  MockInserter* mi = be.last_inserter;
  check(mi && mi->pushed.size() == 1, "inserter captured 1 push");
  check(mi && mi->pushed.size() == 1 && mi->pushed[0] == p1, "pushed bytes match");
  check(mi && mi->pts.size() == 1 && mi->pts[0] == 42, "pushed pts match");
  check(mi && mi->finished, "finish recorded");
  const auto polled = (*ins)->poll();
  check(static_cast<bool>(polled), "Inserter poll is a no-op for a synchronous backend");

  std::printf("MockBackend: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
