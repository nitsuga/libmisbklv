// SPDX-License-Identifier: Apache-2.0
// Real-time streaming round-trip (ADR 0013 / B4): KLV packets -> appsrc !
// mpegtsmux ! udpsink --(loopback udp)--> udpsrc ! tsdemux ! appsink -> KLV.
// Proves the live path end-to-end: the sender paces output on the clock
// (realtime), and the receiver's blocking extract() ends on the udpsrc idle
// timeout (no EOS crosses the network). SRT instead stops cooperatively.
// argv: <input.klv> [port] [udp|srt-stop]
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gst/app/app.h>

#include "misbklv/backend.hpp"
#include "misbklv/gst_backend.hpp"
#include "misbklv/packet.hpp"

using namespace misbklv;

static std::vector<std::byte> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
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
  const bool srt_stop = argc >= 4 && std::string(argv[3]) == "srt-stop";
  const std::string endpoint = srt_stop
                                   ? "srt://127.0.0.1:" + std::to_string(port) + "?mode=listener"
                                   : "udp:127.0.0.1:" + std::to_string(port);

  // --- receiver: blocking extract() from the loopback udp port, on a thread ---
  std::vector<std::byte> out;
  std::atomic<bool> extract_ok{false};
  std::atomic<bool> received{false};
  std::stop_source stop;
  std::thread rx([&] {
    auto be = make_gst_backend();
    auto r = be->extract(
        endpoint,
        [&](const KlvPacket& kp) {
          out.insert(out.end(), kp.bytes.begin(), kp.bytes.end());
          received.store(true, std::memory_order_release);
        },
        stop.get_token());
    extract_ok = static_cast<bool>(r);
  });

  // Let the receiver reach PLAYING and bind the udp socket before we send —
  // otherwise the first datagram (carrying PAT/PMT) would be lost.
  std::this_thread::sleep_for(std::chrono::milliseconds(700));

  if (srt_stop) {
    GstElement* sender = gst_pipeline_new("misbklv-srt-test-sender");
    GstElement* appsrc = gst_element_factory_make("appsrc", "src");
    GstElement* sink = gst_element_factory_make("srtsink", "sink");
    if (!sender || !appsrc || !sink) {
      if (sender) gst_object_unref(sender);
      if (appsrc) gst_object_unref(appsrc);
      if (sink) gst_object_unref(sink);
      stop.request_stop();
      rx.join();
      return 2;
    }
    const std::string sink_uri = "srt://127.0.0.1:" + std::to_string(port) + "?mode=caller";
    GstCaps* caps = gst_caps_from_string("video/mpegts, systemstream=(boolean)true");
    g_object_set(appsrc, "caps", caps, "format", GST_FORMAT_TIME, nullptr);
    gst_caps_unref(caps);
    g_object_set(sink, "uri", sink_uri.c_str(), nullptr);
    gst_bin_add_many(GST_BIN(sender), appsrc, sink, nullptr);
    if (!gst_element_link(appsrc, sink) ||
        gst_element_set_state(sender, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      gst_element_set_state(sender, GST_STATE_NULL);
      gst_object_unref(sender);
      stop.request_stop();
      rx.join();
      return 2;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, input.size(), nullptr);
    gst_buffer_fill(buffer, 0, input.data(), input.size());
    if (gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer) != GST_FLOW_OK) {
      gst_element_set_state(sender, GST_STATE_NULL);
      gst_object_unref(sender);
      stop.request_stop();
      rx.join();
      return 2;
    }
    const auto receive_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!received.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < receive_deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto stop_started = std::chrono::steady_clock::now();
    stop.request_stop();  // srtsrc has no idle end; issue #75 exercises this path.
    rx.join();
    const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - stop_started)
                             .count();
    gst_element_set_state(sender, GST_STATE_NULL);
    gst_object_unref(sender);
    if (!received.load(std::memory_order_acquire) || !extract_ok || stop_ms >= 2000) {
      std::fprintf(stderr, "SRT extraction/cancellation failed (%lld ms)\n",
                   static_cast<long long>(stop_ms));
      return 2;
    }
    std::printf("SRT extraction + cancellation: PASS (%lld ms)\n", static_cast<long long>(stop_ms));
    return 0;
  }

  // --- sender: push each framed KLV packet, clock-paced (realtime) ------------
  auto be = make_gst_backend();
  auto ins = be->open_insert({endpoint, /*realtime=*/true, /*video_source=*/"", Sei0604::Preserve});
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
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
          .count();
  std::printf("streamed %zu packets over udp:%d in %lld ms (clock-paced)\n", npush, port,
              static_cast<long long>(elapsed));
  rx.join();  // UDP ends on idle timeout.
  if (!extract_ok) {
    std::fprintf(stderr, "receiver extract failed\n");
    return 2;
  }
  std::printf("received %zu bytes (input %zu)\n", out.size(), input.size());
  const bool match = (out == input);
  std::printf("STREAM ROUND-TRIP: %s\n", match ? "byte-exact PASS" : "MISMATCH");
  return match ? 0 : 1;
}
