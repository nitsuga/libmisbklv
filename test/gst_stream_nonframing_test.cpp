// SPDX-License-Identifier: Apache-2.0
// Live-path non-framing feed (issue #4, verification bullet 2): a KLV PID that
// never carries the SMPTE UL — the misconfigured-source / wrong-port scenario
// the PtsMarks::prune fix targets — must not hang, crash, or misbehave. push()
// puts arbitrary bytes on the wire (it does not require well-formed KLV), so
// this drives a real gstreamer pipeline: appsrc ! mpegtsmux ! udpsink -->
// udpsrc ! tsdemux ! appsink, sending many buffers of UL-free filler over a
// realtime (clock-paced) pipeline. extract() must still end cleanly on the
// udpsrc idle timeout, having framed nothing.
// argv: [port]
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "misbklv/backend.hpp"
#include "misbklv/gst_backend.hpp"
#include "misbklv/packet.hpp"

using namespace misbklv;

int main(int argc, char** argv) {
  const int port = (argc >= 2) ? std::atoi(argv[1]) : 15005;
  const std::string endpoint = "udp:127.0.0.1:" + std::to_string(port);

  // --- receiver: blocking extract() from the loopback udp port, on a thread ---
  std::vector<std::byte> out;
  std::size_t nframed = 0;
  std::atomic<bool> extract_ok{false};
  std::thread rx([&] {
    auto be = make_gst_backend();
    auto r = be->extract(endpoint, [&](const KlvPacket& kp) {
      out.insert(out.end(), kp.bytes.begin(), kp.bytes.end());
      ++nframed;
    });
    extract_ok = static_cast<bool>(r);
  });

  // Let the receiver reach PLAYING and bind the udp socket before we send —
  // otherwise the first datagrams would be lost.
  std::this_thread::sleep_for(std::chrono::milliseconds(700));

  // --- sender: push UL-free filler, clock-paced (realtime) ---------------------
  // A single repeated byte can never contain the 4-distinct-byte SMPTE UL
  // prefix (06 0e 2b 34), so this PID is selected as the KLV PID (it's the
  // only metadata stream) but never frames a packet — every push() grows
  // PtsMarks by one entry pre-fix, forever, since drain() never completes a
  // frame and so never calls marks.at().
  auto be = make_gst_backend();
  auto ins = be->open_insert({endpoint, /*realtime=*/true, /*video_source=*/"",
                              Sei0604::Preserve});
  if (!ins) {
    std::fprintf(stderr, "open_insert failed: %d\n", static_cast<int>(ins.error()));
    rx.join();
    return 2;
  }
  constexpr std::size_t kBufSize = 512;
  constexpr int kBuffers = 90;  // ~3s at the ~33ms default frame pacing
  std::vector<std::byte> filler(kBufSize, std::byte{0xEE});
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kBuffers; ++i) {
    if (!(*ins)->push(filler, kNoPts)) {
      std::fprintf(stderr, "push failed at buffer %d\n", i);
      rx.join();
      return 2;
    }
  }
  if (!(*ins)->finish()) {
    std::fprintf(stderr, "finish failed\n");
    rx.join();
    return 2;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
  std::printf("streamed %d non-framing buffers over udp:%d in %lld ms\n", kBuffers,
              port, static_cast<long long>(elapsed));

  rx.join();  // returns once the udpsrc idle timeout fires (post-stream)
  if (!extract_ok) {
    std::fprintf(stderr, "receiver extract failed\n");
    return 1;
  }
  std::printf("framed %zu packets (expected 0), %zu bytes\n", nframed, out.size());
  const bool ok = extract_ok && nframed == 0 && out.empty();
  std::printf("NON-FRAMING LIVE PATH: %s\n", ok ? "PASS (ended cleanly)" : "FAIL");
  return ok ? 0 : 1;
}
