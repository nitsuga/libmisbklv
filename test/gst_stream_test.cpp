// SPDX-License-Identifier: Apache-2.0
// Real-time streaming round-trip (ADR 0013 / B4): KLV packets -> appsrc !
// mpegtsmux ! udpsink --(loopback udp)--> udpsrc ! tsdemux ! appsink -> KLV.
// Proves the live path end-to-end: the sender paces output on the clock
// (realtime), and the receiver's blocking extract() ends on the udpsrc idle
// timeout (no EOS crosses the network). The recovered KLV must be byte-exact.
// argv: <input.klv> [port]
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "misbklv/backend.hpp"
#include "misbklv/gst_backend.hpp"
#include "misbklv/packet.hpp"

using namespace misbklv;

static std::vector<std::byte> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i)
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: gst_stream_test <input.klv> [port]\n");
    return 2;
  }
  const auto input = read_file(argv[1]);
  std::span<const std::byte> buf = input;
  const int port = (argc >= 3) ? std::atoi(argv[2]) : 15004;
  const std::string endpoint = "udp:127.0.0.1:" + std::to_string(port);

  // --- receiver: blocking extract() from the loopback udp port, on a thread ---
  std::vector<std::byte> out;
  std::atomic<bool> extract_ok{false};
  std::thread rx([&] {
    auto be = make_gst_backend();
    auto r = be->extract(endpoint, [&](const KlvPacket& kp) {
      out.insert(out.end(), kp.bytes.begin(), kp.bytes.end());
    });
    extract_ok = static_cast<bool>(r);
  });

  // Let the receiver reach PLAYING and bind the udp socket before we send —
  // otherwise the first datagram (carrying PAT/PMT) would be lost.
  std::this_thread::sleep_for(std::chrono::milliseconds(700));

  // --- sender: push each framed KLV packet, clock-paced (realtime) ------------
  auto be = make_gst_backend();
  auto ins = be->open_insert({endpoint, /*realtime=*/true, /*video_source=*/"",
                              Sei0604::Preserve});
  if (!ins) {
    std::fprintf(stderr, "open_insert failed: %d\n", static_cast<int>(ins.error()));
    rx.join();
    return 2;
  }
  const auto t0 = std::chrono::steady_clock::now();
  std::size_t off = 0, npush = 0;
  while (off < input.size()) {
    const std::size_t n = packet_frame_length(buf.subspan(off));
    if (n == 0) break;
    if (!(*ins)->push(buf.subspan(off, n), kNoPts)) {
      std::fprintf(stderr, "push failed at packet %zu\n", npush);
      rx.join();
      return 2;
    }
    off += n;
    ++npush;
  }
  if (!(*ins)->finish()) {
    std::fprintf(stderr, "finish failed\n");
    rx.join();
    return 2;
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count();
  std::printf("streamed %zu packets over udp:%d in %lld ms (clock-paced)\n", npush,
              port, static_cast<long long>(elapsed));

  rx.join();  // returns once the udpsrc idle timeout fires (post-stream)
  if (!extract_ok) {
    std::fprintf(stderr, "receiver extract failed\n");
    return 2;
  }
  std::printf("received %zu bytes (input %zu)\n", out.size(), input.size());
  const bool match = (out == input);
  std::printf("STREAM ROUND-TRIP: %s\n", match ? "byte-exact PASS" : "MISMATCH");
  return match ? 0 : 1;
}
