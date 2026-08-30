// SPDX-License-Identifier: Apache-2.0
// Insertion round-trip (ADR 0013 / B2): KLV packets -> appsrc ! mpegtsmux !
// filesink -> .ts -> re-extract -> byte-exact. Proves stock mpegtsmux carries
// KLV (0x06+KLVA) losslessly, so no klvpmtrewrite is needed.
// argv: <input.klv> <temp.ts>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <gst/app/app.h>
#include <gst/gst.h>

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

// Write deliberately unframed KLV elementary-stream bytes through the same
// appsrc ! mpegtsmux path as production insertion. Unlike Inserter::push(),
// this lets the extraction test put corrupt bytes before a real packet.
static bool mux_chunks(const char* path, std::span<const std::byte> first,
                       std::span<const std::byte> second, std::span<const std::byte> third) {
  gst_init(nullptr, nullptr);
  GstElement* pipeline = gst_pipeline_new("misbklv-resync-test");
  GstElement* src = gst_element_factory_make("appsrc", "src");
  GstElement* mux = gst_element_factory_make("mpegtsmux", "mux");
  GstElement* sink = gst_element_factory_make("filesink", "sink");
  if (!pipeline || !src || !mux || !sink) {
    if (sink) gst_object_unref(sink);
    if (mux) gst_object_unref(mux);
    if (src) gst_object_unref(src);
    if (pipeline) gst_object_unref(pipeline);
    return false;
  }
  GstCaps* caps = gst_caps_from_string("meta/x-klv, parsed=(boolean)true");
  g_object_set(src, "caps", caps, "format", GST_FORMAT_TIME, "block", TRUE, "is-live", FALSE,
               nullptr);
  g_object_set(sink, "location", path, nullptr);
  gst_caps_unref(caps);
  gst_bin_add_many(GST_BIN(pipeline), src, mux, sink, nullptr);
  if (!gst_element_link_many(src, mux, sink, nullptr) ||
      gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return false;
  }

  bool ok = true;
  std::size_t chunk_index = 0;
  for (const auto chunk : {first, second, third}) {
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, chunk.size(), nullptr);
    gst_buffer_fill(buffer, 0, chunk.data(), chunk.size());
    GST_BUFFER_PTS(buffer) = chunk_index * GST_SECOND;
    GST_BUFFER_DURATION(buffer) = GST_SECOND;
    if (gst_app_src_push_buffer(GST_APP_SRC(src), buffer) != GST_FLOW_OK) ok = false;
    ++chunk_index;
  }
  if (gst_app_src_end_of_stream(GST_APP_SRC(src)) != GST_FLOW_OK) ok = false;
  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* msg = gst_bus_timed_pop_filtered(
      bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  ok = ok && msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
  if (msg) gst_message_unref(msg);
  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  return ok;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: gst_insert_test <input.klv> <temp.ts>\n");
    return 2;
  }
  const auto input = read_file(argv[1]);
  std::span<const std::byte> buf = input;
  auto be = make_gst_backend();

  // --- insert: push each framed KLV packet through the mux to a .ts ---------
  auto ins = be->open_insert({std::string("file:") + argv[2], /*realtime=*/false,
                              /*video_source=*/"", Sei0604::Preserve});
  if (!ins) {
    std::fprintf(stderr, "open_insert failed: %d\n", static_cast<int>(ins.error()));
    return 2;
  }
  std::size_t off = 0, npush = 0;
  const std::size_t insert_packet_size = packet_frame_length(buf);
  const auto bad_span = (*ins)->push(buf.first(insert_packet_size), kNoPts - 1);
  const bool bad_span_ok = !bad_span && bad_span.error() == Error::RangeError;
  std::printf("INSERT NEGATIVE PTS (span): %s\n", bad_span_ok ? "PASS" : "MISMATCH");
  if (!bad_span_ok) return 1;
  std::vector<std::byte> bad_vector(buf.first(insert_packet_size).begin(),
                                    buf.first(insert_packet_size).end());
  const auto bad_move = (*ins)->push(std::move(bad_vector), kNoPts - 1);
  const bool bad_move_ok = !bad_move && bad_move.error() == Error::RangeError;
  std::printf("INSERT NEGATIVE PTS (vector): %s\n", bad_move_ok ? "PASS" : "MISMATCH");
  if (!bad_move_ok) return 1;
  const std::string sentinel_path = std::string(argv[2]) + ".sentinel.ts";
  auto sentinel =
      be->open_insert({std::string("file:") + sentinel_path, false, "", Sei0604::Preserve});
  if (!sentinel) return 1;
  const auto untimed_span = (*sentinel)->push(buf.first(insert_packet_size), kNoPts);
  std::vector<std::byte> untimed_vector(buf.first(insert_packet_size).begin(),
                                        buf.first(insert_packet_size).end());
  const auto untimed_move = (*sentinel)->push(std::move(untimed_vector), kNoPts);
  const bool untimed_ok = untimed_span && untimed_move && (*sentinel)->finish();
  std::remove(sentinel_path.c_str());
  if (!untimed_ok) return 1;
  while (off < input.size()) {
    const std::size_t n = packet_frame_length(buf.subspan(off));
    if (n == 0) break;
    if (!(*ins)->push(buf.subspan(off, n), kNoPts)) {
      std::fprintf(stderr, "push failed at packet %zu\n", npush);
      return 2;
    }
    off += n;
    ++npush;
  }
  if (!(*ins)->finish()) {
    std::fprintf(stderr, "finish failed\n");
    return 2;
  }
  std::printf("inserted %zu packets -> %s\n", npush, argv[2]);

  // --- re-extract and compare ----------------------------------------------
  std::vector<std::byte> out;
  auto r = be->extract(argv[2], [&](const KlvPacket& kp) {
    out.insert(out.end(), kp.bytes.begin(), kp.bytes.end());
  });
  if (!r) {
    std::fprintf(stderr, "re-extract failed: %d\n", static_cast<int>(r.error()));
    return 2;
  }
  std::printf("re-extracted %zu bytes (input %zu)\n", out.size(), input.size());
  const bool match = (out == input);
  std::printf("INSERT ROUND-TRIP: %s\n", match ? "byte-exact PASS" : "MISMATCH");
  if (!match) return 1;

  // Incremental extraction refuses an oversized declared frame before delivering
  // it. The cap is one byte below the first real packet, so no callback is allowed.
  const std::size_t first_packet_size = packet_frame_length(input);
  if (first_packet_size == 0) {
    std::fprintf(stderr, "fixture has no complete first KLV packet\n");
    return 2;
  }
  std::size_t over_limit_callbacks = 0;
  auto over_limit = be->extract(
      argv[2], [&](const KlvPacket&) { ++over_limit_callbacks; }, {},
      ExtractOptions{.max_packet_bytes = first_packet_size - 1});
  const bool limit_ok =
      !over_limit && over_limit.error() == Error::ResourceLimit && over_limit_callbacks == 0;
  std::printf("EXTRACT LIMIT: %s\n", limit_ok ? "PASS" : "MISMATCH");
  if (!limit_ok) return 1;

  // --- corrupt ES prefix: extractor must discard it and resynchronize --------
  const std::size_t valid_size = first_packet_size;
  std::vector<std::byte> garbage(17, std::byte{0xA5});
  garbage[16] = std::byte{0x00};  // superficially complete but not a known UL
  const auto valid = std::span<const std::byte>(input).first(valid_size);
  const std::size_t split = 3;  // split the SMPTE UL prefix (06 0e 2b | 34...)
  const std::string resync_path = std::string(argv[2]) + ".resync.ts";
  if (!mux_chunks(resync_path.c_str(), garbage, valid.first(split), valid.subspan(split))) {
    std::fprintf(stderr, "resync test mux failed\n");
    return 2;
  }
  std::vector<std::byte> resynced;
  std::size_t emitted = 0;
  auto resync = be->extract(resync_path, [&](const KlvPacket& kp) {
    resynced.insert(resynced.end(), kp.bytes.begin(), kp.bytes.end());
    ++emitted;
  });
  const bool resync_ok = resync && emitted == 1 && resynced.size() == valid.size() &&
                         std::equal(resynced.begin(), resynced.end(), valid.begin());
  std::printf("EXTRACT RESYNC: %s (emitted=%zu bytes=%zu expected=%zu error=%d)\n",
              resync_ok ? "PASS" : "MISMATCH", emitted, resynced.size(), valid.size(),
              resync ? 0 : static_cast<int>(resync.error()));
  if (!resync_ok) return 1;

  // --- natural EOS with a declared frame missing its last byte is truncated ---
  const auto partial = valid.first(valid.size() - 1);
  const std::size_t cut1 = std::min<std::size_t>(3, partial.size());
  const std::size_t cut2 = std::min<std::size_t>(6, partial.size());
  const std::string truncated_path = std::string(argv[2]) + ".truncated.ts";
  if (!mux_chunks(truncated_path.c_str(), partial.first(cut1), partial.subspan(cut1, cut2 - cut1),
                  partial.subspan(cut2))) {
    std::fprintf(stderr, "truncated test mux failed\n");
    return 2;
  }
  std::size_t truncated_callbacks = 0;
  auto truncated = be->extract(truncated_path, [&](const KlvPacket&) { ++truncated_callbacks; });
  const bool truncated_ok =
      !truncated && truncated.error() == Error::Truncated && truncated_callbacks == 0;
  std::printf("EXTRACT TRUNCATED EOS: %s\n", truncated_ok ? "PASS" : "MISMATCH");
  return truncated_ok ? 0 : 1;
}
