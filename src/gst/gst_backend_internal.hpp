// SPDX-License-Identifier: Apache-2.0
// Private seams for the gstreamer backend. Not installed.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gst/gst.h>
#define GST_USE_UNSTABLE_API
#include <gst/codecparsers/gsth264parser.h>
#undef GST_USE_UNSTABLE_API

#include "misbklv/backend.hpp"

namespace misbklv::detail {

Result<std::monostate> extract(std::string_view source, const PacketHandler& on_packet,
                               std::stop_token stop, ExtractOptions options);
Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig& cfg);

// "[::1]:5000" / "host:5000" -> host + port, or nullopt if malformed.
std::optional<std::pair<std::string, int>> parse_host_port(std::string_view rest);

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

// Push EOS from an unbounded live branch only after acquiring `source_pad`'s
// and the linked `mux_pad`'s stream locks in source-to-sink order. The
// source/ghost lock can be idle while a downstream chain function holds the mux
// lock, so both must be acquired before entering gst_pad_push_event(). The
// caller owns both pads and already holds a ref on `source_pad`. Exposed only
// through this private header so lock selection and timeout behavior can be
// tested.
bool push_live_eos_when_idle(GstPad* source_pad, GstPad* mux_pad, std::stop_token stop,
                             std::chrono::steady_clock::duration lock_timeout);

enum class CodecLatch { Unknown, IsH264, NotH264 };

struct VideoCtx {
  GstElement* pipeline = nullptr;  // for creating fakesinks
  // The muxer sink pad reserved for video while the pipeline was still NULL,
  // so it takes the lower ES PID and is announced first in the PMT (ADR 0020
  // § stream order). Borrowed: the muxer owns the pad; this is only linked.
  GstPad* reserved_video_pad = nullptr;
  // Unbounded live branches never EOS on their own; finish() pushes EOS through
  // their final src pad while it remains linked so mpegtsmux can drain safely.
  bool is_live_unbounded = false;  // true if no num-buffers (pipeline) or RTSP
  // Updated by a persistent probe on the reserved mux sink pad to expose
  // source progress. `delivered_buffer` separately preserves the exact
  // first-buffer readiness latch used by live close.
  std::atomic<std::chrono::steady_clock::rep> last_delivery_ticks{0};
  std::atomic<bool> delivered_buffer{false};
  // Set as soon as rtspsrc announces any pad, which means the server answered
  // and described its streams. It separates "nothing there" from "there, but
  // carrying media this build cannot handle" when no video pad ever links
  // (ADR 0036).
  std::atomic<bool> saw_any_pad{false};
  GstElement* mux_element = nullptr;  // borrowed, owned by pipeline
  GstElement* video_bin = nullptr;    // borrowed: rtspsrc or pipeline bin
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

  // Every probe holding a raw `VideoCtx*`: the live delivery tracker plus the
  // CAPS-event/buffer probes armed by attach_generate_probes. A callback after
  // this object is freed is a use-after-free (issue #57), so every probe is
  // recorded and severed before NULL/destruction rather than relying on a state
  // transition to have quiesced streaming threads. Each entry owns a pad ref so
  // removal stays valid mid-teardown; guarded by `probe_mu`.
  std::mutex probe_mu;
  std::vector<std::pair<GstPad*, gulong>> probes;
  bool probes_severed = false;

  // Record a just-armed probe. Refs `pad`. Thread-safe: pad-added callbacks arm
  // probes on streaming threads while teardown runs on the caller's. If teardown
  // has already severed, the probe is removed immediately here so a late live
  // pad cannot re-arm the race.
  void register_probe(GstPad* pad, gulong id);
  // Remove every recorded probe and drop the pad refs, then mark severed.
  // gst_pad_remove_probe blocks until any in-flight callback returns, so after
  // this call no probe callback can be running or start — safe to free the ctx.
  // Idempotent.
  void remove_probes();

  ~VideoCtx();
};

// Classify a GStreamer error from an RTSP branch. Only a resource-level
// failure means the source is absent right now and is worth retrying; a stream
// or core error is this build or this server's media being unusable, which
// retrying cannot fix. Authorization is resource-level but permanent — wrong
// credentials stay wrong (ADR 0036). Exposed for tests: the permanent paths
// need a live server to reach otherwise.
// `rtsp_status` is the RTSP response status from the error message's details
// (`rtsp-status-code`), or 0 when the failure never got that far — a refused
// TCP connection carries no status. A status means the server answered, so it
// is not absent, and only a server-side 5xx is worth coming back to (ADR 0036).
Error classify_rtsp_error(GQuark domain, int code, int rtsp_status = 0);

// Pull `rtsp-status-code` out of an error message's details, or 0 if absent.
int rtsp_status_from_message(GstMessage* msg);

// True when `src` is `branch` or lives inside it. The RTSP branch watches the
// whole pipeline bus, so an error posted by the sink or the muxer arrives on
// the same bus as one from rtspsrc; only the latter says anything about the
// source (ADR 0036).
bool object_within_branch(GstObject* src, GstElement* branch);

// Adds and prerolls the video branch. On an error the caller retains `video`
// until it has taken the pipeline to NULL, keeping all callback user pointers
// valid during teardown.
// `reserved_video_pad` is the muxer sink pad reserved for video while the
// pipeline was NULL (borrowed, owned by the muxer); the video stream is linked
// onto it once the demuxer exposes its pad, keeping video first in the PMT.
// `mux` is the pipeline's mpegtsmux element (borrowed, stored in VideoCtx).
Result<std::monostate> prepare_video_branch(GstElement* pipeline, GstPad* reserved_video_pad,
                                            GstElement* mux, const VideoSource& src,
                                            Sei0604 sei_0604, std::unique_ptr<VideoCtx>& video);

void record_sensor_timestamp(VideoCtx& video, std::span<const std::byte> pkt, std::int64_t pts_ns);

}  // namespace misbklv::detail
