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
  auto r = be.extract("mock", [&](const KlvPacket& kp) {
    got.emplace_back(kp.bytes.begin(), kp.bytes.end());
  });
  check(bool(r), "extract returns ok");
  check(got.size() == 2, "extract yielded 2 packets");
  check(got.size() == 2 && got[0] == p1 && got[1] == p2, "extract bytes match canned");

  auto ins = be.open_insert({"file:out.ts"});
  check(bool(ins), "open_insert ok");
  (void)(*ins)->push(std::span<const std::byte>(p1), 42);
  (void)(*ins)->finish();
  MockInserter* mi = be.last_inserter;
  check(mi && mi->pushed.size() == 1, "inserter captured 1 push");
  check(mi && mi->pushed.size() == 1 && mi->pushed[0] == p1, "pushed bytes match");
  check(mi && mi->pts.size() == 1 && mi->pts[0] == 42, "pushed pts match");
  check(mi && mi->finished, "finish recorded");

  std::printf("MockBackend: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
