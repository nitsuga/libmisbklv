// SPDX-License-Identifier: Apache-2.0
// Private seams for the gstreamer backend. Not installed.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <gst/gst.h>
#define GST_USE_UNSTABLE_API
#include <gst/codecparsers/gsth264parser.h>
#undef GST_USE_UNSTABLE_API

#include "misbklv/backend.hpp"

namespace misbklv::detail {

Result<std::monostate> extract(std::string_view source,
                               const PacketHandler& on_packet,
                               std::stop_token stop, ExtractOptions options);
Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig& cfg);

// Builds the pipeline sink element for a sink spec string ("file:PATH",
// "udp:host:port", "srt:uri"). Returns null on a malformed spec. Non-static so
// the unit tests can construct a sink and inspect its udpsink properties
// without spinning up a full pipeline; not part of the installed API.
GstElement* make_sink(const std::string& spec, const InsertConfig& cfg);

struct SensorTime {
  std::uint64_t timestamp_us = 0;
  std::uint8_t status = 0;
};

enum class VideoSourceKind { None, File, Rtsp, Pipeline, Unsupported };

struct VideoSource {
  VideoSourceKind kind = VideoSourceKind::None;
  std::string spec;  // File: stripped path; Rtsp: full URI; Pipeline: desc after "pipeline:"
};

VideoSource parse_video_source(const std::string& raw);

enum class CodecLatch { Unknown, IsH264, NotH264 };

struct VideoCtx {
  GstElement* pipeline = nullptr;  // for creating fakesinks
  // The muxer sink pad reserved for video while the pipeline was still NULL,
  // so it takes the lower ES PID and is announced first in the PMT (ADR 0020
  // § stream order). Borrowed: the muxer owns the pad; this is only linked.
  GstPad* reserved_video_pad = nullptr;
  // Unbounded live branches never EOS on their own; finish() pushes EOS through
  // their final src pad while it remains linked so mpegtsmux can drain safely.
  bool is_live = false;
  bool is_live_unbounded = false; // true if no num-buffers (pipeline) or RTSP
  GstElement* mux_element = nullptr;   // borrowed, owned by pipeline
  GstElement* video_bin = nullptr;     // borrowed: rtspsrc or pipeline bin
  std::mutex mu;
  std::condition_variable cv;
  bool linked = false;
  bool no_more_pads = false;
  int ignored_video_pads = 0;
  bool generate_sei = false;

  std::mutex timestamp_mu;
  std::map<std::uint64_t, SensorTime> pts_to_sensor_timestamp;
  bool have_prev_push = false;
  std::uint64_t prev_push_pts_ns = 0;
  std::uint64_t prev_push_ts_us = 0;
  GstH264NalParser* h264_parser = nullptr;
  std::atomic<CodecLatch> codec_latch{CodecLatch::Unknown};
  // Latest TIME segment seen at the Generate probe. Encoders using
  // gst_video_encoder_set_min_pts encode their actual PTS adjustment into the
  // segment; on a direct-space miss, converting through it recovers the
  // caller's running-time timeline even when the source does not start at PTS
  // zero (ADR 0033). All three fields are protected by timestamp_mu.
  GstSegment video_segment{};
  bool have_video_segment = false;
  bool use_segment_timeline = false;

  // SEI-injection probes (the CAPS-event and buffer probes attach_generate_probes
  // arms). Each holds a raw `VideoCtx*` as its user data, so a probe callback
  // that fires after this VideoCtx is freed is a use-after-free — the deferred
  // heap corruption behind issue #57. We record every probe as it is armed and
  // sever them all deterministically at teardown, before the pipeline goes NULL
  // and before this object is destroyed, rather than relying on set_state(NULL)
  // to have quiesced the streaming threads first. Each entry owns a ref on its
  // pad so remove stays valid mid-teardown; guarded by `probe_mu`.
  std::mutex probe_mu;
  std::vector<std::pair<GstPad*, gulong>> sei_probes;
  bool probes_severed = false;

  // Record a just-armed probe. Refs `pad`. Thread-safe: pad-added callbacks arm
  // probes on streaming threads while teardown runs on the caller's. If teardown
  // has already severed, the probe is removed immediately here so a late live
  // pad cannot re-arm the race.
  void register_sei_probe(GstPad* pad, gulong id);
  // Remove every recorded probe and drop the pad refs, then mark severed.
  // gst_pad_remove_probe blocks until any in-flight callback returns, so after
  // this call no probe callback can be running or start — safe to free the ctx.
  // Idempotent.
  void remove_sei_probes();

  ~VideoCtx();
};

// Adds and prerolls the video branch. On an error the caller retains `video`
// until it has taken the pipeline to NULL, keeping all callback user pointers
// valid during teardown.
// `reserved_video_pad` is the muxer sink pad reserved for video while the
// pipeline was NULL (borrowed, owned by the muxer); the video stream is linked
// onto it once the demuxer exposes its pad, keeping video first in the PMT.
// `mux` is the pipeline's mpegtsmux element (borrowed, stored in VideoCtx).
Result<std::monostate> prepare_video_branch(
    GstElement* pipeline, GstPad* reserved_video_pad, GstElement* mux,
    const VideoSource& src, Sei0604 sei_0604,
    std::unique_ptr<VideoCtx>& video);

void record_sensor_timestamp(VideoCtx& video, std::span<const std::byte> pkt,
                             std::int64_t pts_ns);

}  // namespace misbklv::detail
