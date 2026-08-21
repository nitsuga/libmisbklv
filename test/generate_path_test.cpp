// SPDX-License-Identifier: Apache-2.0
// Issue #26: bounded pts_to_sensor_timestamp map and codec latch.
// Direct unit coverage via internal header seam (like udp_multicast_test).
#include <cstdio>
#include <cstdint>
#include <span>
#include <vector>

#include "gst_backend_internal.hpp"
#include "misbklv/message.hpp"
#include "misbklv/packet.hpp"

using namespace misbklv;
using namespace misbklv::detail;

static int failures = 0;
static void check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  } else {
    std::printf("PASS: %s\n", what);
  }
}

static std::vector<std::byte> make_packet(std::uint64_t ts_us) {
  auto msg = Message::create(RegistryId::Uas0601);
  if (!msg) return {};
  (void)msg->set(2, Value{ts_us});
  (void)msg->set(65, Value{std::uint64_t{19}});
  auto enc = msg->encode();
  if (!enc) return {};
  return std::vector<std::byte>(enc->begin(), enc->end());
}

static void test_bounded_map_monotonic() {
  std::printf("== bounded-map: monotonic pushes stay bounded ==\n");
  VideoCtx ctx;
  // Push thousands monotonically at ~30 Hz (33 ms steps).
  constexpr int kCount = 10000;
  constexpr std::int64_t kStepNs = 33'333'333;
  for (int i = 0; i < kCount; ++i) {
    auto pkt = make_packet(1'600'000'000'000'000ULL + static_cast<std::uint64_t>(i) * 33'333);
    if (pkt.empty()) { check(false, "packet encode"); return; }
    record_sensor_timestamp(ctx, pkt, static_cast<std::int64_t>(i) * kStepNs);
    // Intermediate bound check every 1000
    if ((i + 1) % 1000 == 0) {
      std::lock_guard<std::mutex> lk(ctx.timestamp_mu);
      size_t sz = ctx.pts_to_sensor_timestamp.size();
      // 1 s window at 30 Hz => ~30 entries, allow 35
      char buf[128];
      std::snprintf(buf, sizeof(buf), "bounded after %d pushes (size %zu <=35)", i + 1, sz);
      check(sz <= 35, buf);
      if (sz > 35) {
        std::fprintf(stderr, "  size %zu exceeds bound at i=%d\n", sz, i);
      }
    }
  }
  {
    std::lock_guard<std::mutex> lk(ctx.timestamp_mu);
    size_t sz = ctx.pts_to_sensor_timestamp.size();
    std::printf("  final map size %zu after %d pushes\n", sz, kCount);
    check(sz <= 35, "final size bounded to prune window (~1 s)");
    check(sz > 0, "map not empty after pushes");
  }
  // Verify latest entry still matches within tolerance
  {
    std::lock_guard<std::mutex> lk(ctx.timestamp_mu);
    auto it = ctx.pts_to_sensor_timestamp.upper_bound(static_cast<std::uint64_t>(kCount * kStepNs));
    if (it != ctx.pts_to_sensor_timestamp.begin()) {
      --it;
      std::int64_t last_pts = static_cast<std::int64_t>((kCount - 1) * kStepNs);
      std::int64_t diff = static_cast<std::int64_t>(kCount * kStepNs) - static_cast<std::int64_t>(it->first);
      check(diff <= 1'000'000'000LL, "latest entry within prune window");
      (void)last_pts;
    } else {
      check(false, "upper_bound found entry");
    }
    // Old entry 0 should have been pruned
    auto found = ctx.pts_to_sensor_timestamp.find(0);
    check(found == ctx.pts_to_sensor_timestamp.end(), "entry at pts 0 pruned");
  }
}

static void test_codec_latch_initial() {
  std::printf("== codec latch: initial Unknown ==\n");
  VideoCtx ctx;
  check(ctx.codec_latch.load() == CodecLatch::Unknown, "initial latch Unknown");
  ctx.codec_latch.store(CodecLatch::IsH264);
  check(ctx.codec_latch.load() == CodecLatch::IsH264, "latch IsH264");
  ctx.codec_latch.store(CodecLatch::NotH264);
  check(ctx.codec_latch.load() == CodecLatch::NotH264, "latch NotH264");
  // Renegotiation latches again
  ctx.codec_latch.store(CodecLatch::IsH264);
  check(ctx.codec_latch.load() == CodecLatch::IsH264, "latch renegotiation to IsH264");
}

int main() {
  test_bounded_map_monotonic();
  test_codec_latch_initial();
  std::printf("\nGENERATE_PATH: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
