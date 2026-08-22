// Issue #57 regression guard: the SEI-injection pad probes must never touch a
// freed VideoCtx during pipeline teardown.
//
// This exercises the case the `live_rtp`/`live_video` suites structurally can't:
// they tear down only after EOS/drain, when no probe is in flight. Here we
// destroy the Inserter *mid-stream* — a live H.264 source with Sei0604::Generate
// (so on_h264_buffer_inject_sei / on_sei_event_probe fire per buffer on the
// streaming thread) behind a leaky queue (so the encoder keeps pushing through
// the probe even while the muxer backpressures), with no finish()/drain. If a
// probe callback outlives the VideoCtx free, it reads/locks freed memory.
//
// Bare, this guards against a hard crash on teardown. The real teeth are under a
// memory checker: `ctest` runs it through valgrind when one is available (see
// CMakeLists.txt), which reports the use-after-free deterministically. Pre-fix
// code fails here with ~50 invalid accesses into a freed VideoCtx; the fix is
// clean.
//
// Usage: teardown_probe_uaf_test [iters] [stream_ms]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include <gst/gst.h>

#include "misbklv/backend.hpp"
#include "misbklv/gst_backend.hpp"
#include "misbklv/message.hpp"
#include "misbklv/types.hpp"

using namespace misbklv;
using namespace std::chrono_literals;

static std::vector<std::byte> make_pkt(std::uint64_t ts_us) {
  auto m = Message::create(RegistryId::Uas0601);
  if (!m) return {};
  (void)m->set(2, Value{ts_us});
  (void)m->set(65, Value{std::uint64_t{19}});
  auto enc = m->encode();
  if (!enc) return {};
  return std::vector<std::byte>(enc->begin(), enc->end());
}

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  const int iters = argc > 1 ? std::atoi(argv[1]) : 8;
  const int base_ms = argc > 2 ? std::atoi(argv[2]) : 200;

  int opened = 0, torn = 0;
  for (int i = 0; i < iters; ++i) {
    auto backend = make_gst_backend();
    if (!backend) continue;

    InsertConfig cfg;
    cfg.sink = "file:teardown_probe_uaf.ts";
    cfg.realtime = false;
    // Leaky queue: h264parse keeps firing the SEI probe on the streaming thread
    // even when the muxer backpressures, so a probe is in flight at teardown.
    cfg.video_source =
        "pipeline:videotestsrc is-live=true ! videoconvert ! "
        "video/x-raw,format=I420,width=320,height=240,framerate=30/1 ! "
        "openh264enc ! h264parse ! "
        "queue leaky=downstream max-size-buffers=3 max-size-bytes=0 "
        "max-size-time=0";
    cfg.sei_0604 = Sei0604::Generate;

    // Jitter the teardown point across the ~33ms frame cycle.
    const int stream_ms = base_ms + (i % 37);
    {
      auto r = backend->open_insert(cfg);
      if (!r) continue;  // encoder/plugin absent on this host — skip this iter
      ++opened;
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(stream_ms);
      int k = 0;
      while (std::chrono::steady_clock::now() < deadline) {
        auto pkt = make_pkt(1'700'000'000'000'000ULL +
                            static_cast<std::uint64_t>(k) * 33'333);
        if (!pkt.empty())
          (void)(*r)->push(pkt, static_cast<std::int64_t>(k + 1) * 33'333'333LL);
        ++k;
        std::this_thread::sleep_for(15ms);
      }
      // Mid-stream teardown: leaving this block runs ~Inserter with the
      // streaming thread still encoding and firing the SEI probe.
    }
    ++torn;
  }

  std::fprintf(stderr, "teardown_probe_uaf: iters=%d opened=%d torn_down=%d\n",
               iters, opened, torn);
  // If no pipeline ever opened (no H.264 encoder on this host) the teardown path
  // was never exercised — report skip rather than a false pass.
  if (opened == 0) {
    std::fprintf(stderr,
                 "  SKIP: no H.264 encoder pipeline could be opened\n");
    return 0;
  }
  std::fprintf(stderr, "  ok: %d mid-stream teardowns, no crash\n", torn);
  return 0;
}
