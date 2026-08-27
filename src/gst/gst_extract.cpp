// SPDX-License-Identifier: Apache-2.0
// GStreamer extraction: source setup, TS demux, incremental KLV framing, and
// live cancellation/termination. Private implementation of MediaBackend.
#include "gst_backend_internal.hpp"

#include <atomic>
#include <cstring>
#include <optional>

#include <gst/app/app.h>

#include "../klv_framer.hpp"

namespace misbklv::detail {
namespace {

// Reassembly + framing state shared with appsink callbacks. The callbacks run
// on a streaming thread while extract() blocks on the bus.
struct ExtractCtx {
  explicit ExtractCtx(std::size_t max_packet_bytes) : framer(max_packet_bytes) {}

  GstElement* sink = nullptr;
  const PacketHandler* on_packet = nullptr;
  KlvFramer framer;
  std::atomic<bool> received_bytes{false};
  Error frame_error = Error::Backend;
  std::atomic<bool> has_frame_error{false};
};

void on_pad_added(GstElement*, GstPad* pad, gpointer user) {
  auto* ctx = static_cast<ExtractCtx*>(user);
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) caps = gst_pad_query_caps(pad, nullptr);
  // Both queries can come back NULL or empty; gst_caps_get_structure would then
  // return NULL and gst_structure_get_name dereference it. Guard as the sibling
  // on_video_pad_added (gst_video.cpp) does before inspecting the caps.
  if (!caps || gst_caps_is_empty(caps)) {
    if (caps) gst_caps_unref(caps);
    return;
  }
  const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  if (name && std::strcmp(name, "meta/x-klv") == 0) {
    GstPad* sinkpad = gst_element_get_static_pad(ctx->sink, "sink");
    if (!gst_pad_is_linked(sinkpad)) gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
  }
  gst_caps_unref(caps);
}

GstFlowReturn on_new_sample(GstElement* sink, gpointer user) {
  auto* ctx = static_cast<ExtractCtx*>(user);
  GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
  if (!sample) return GST_FLOW_OK;
  if (ctx->has_frame_error.load(std::memory_order_acquire)) {
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }
  GstBuffer* buf = gst_sample_get_buffer(sample);
  GstMapInfo mi;
  if (buf && gst_buffer_map(buf, &mi, GST_MAP_READ)) {
    // Record running time before these bytes lose their buffer identity. The
    // demuxer's segment spans the program, so running time is nanoseconds from
    // source start—the same timeline insertion consumes (ADR 0021).
    std::int64_t pts_ns = kNoPts;
    const GstSegment* seg = gst_sample_get_segment(sample);
    if (seg && GST_BUFFER_PTS_IS_VALID(buf)) {
      const GstClockTime rt =
          gst_segment_to_running_time(seg, GST_FORMAT_TIME, GST_BUFFER_PTS(buf));
      if (GST_CLOCK_TIME_IS_VALID(rt)) pts_ns = static_cast<std::int64_t>(rt);
    }
    if (mi.size != 0) ctx->received_bytes.store(true, std::memory_order_release);
    const auto* p = reinterpret_cast<const std::byte*>(mi.data);
    auto frame_error = ctx->framer.feed({p, mi.size}, pts_ns, *ctx->on_packet);
    gst_buffer_unmap(buf, &mi);
    if (frame_error) {
      ctx->frame_error = *frame_error;
      ctx->has_frame_error.store(true, std::memory_order_release);
    }
  }
  gst_sample_unref(sample);
  return ctx->has_frame_error.load(std::memory_order_acquire) ? GST_FLOW_ERROR : GST_FLOW_OK;
}

// UDP has no EOS. Once metadata bytes have arrived, this idle interval is the
// natural end signal. SRT has no equivalent idle message.
inline constexpr guint64 kIdleTimeoutNs = 500'000'000;

// Build a file/UDP/SRT source. `udp` is true only for udpsrc because only it
// posts the timeout message handled by extract().
GstElement* make_src(const std::string& spec, bool* udp) {
  *udp = false;
  const auto colon = spec.find(':');
  const std::string scheme = (colon == std::string::npos) ? std::string() : spec.substr(0, colon);
  if (scheme == "udp") {
    const std::string rest = spec.substr(colon + 1);
    std::string host;
    std::string port_str;
    if (!rest.empty() && rest[0] == '[') {
      // Bracketed IPv6: [::1]:5000 or [ff02::1%eth0]:5000.
      auto close = rest.find(']');
      if (close == std::string::npos) return nullptr;
      if (close + 1 >= rest.size() || rest[close + 1] != ':') return nullptr;
      host = rest.substr(1, close - 1);
      port_str = rest.substr(close + 2);
    } else {
      auto p = rest.rfind(':');
      if (p == std::string::npos) return nullptr;
      // Reject a bare IPv6 literal without brackets: its colons make the
      // last-colon split ambiguous, so refuse rather than mis-parse.
      const std::string host_part = rest.substr(0, p);
      if (host_part.find(':') != std::string::npos) return nullptr;
      host = host_part;
      port_str = rest.substr(p + 1);
    }
    if (host.empty() || port_str.empty()) return nullptr;
    // Validate the port is a positive in-range integer before configuring.
    int port = 0;
    try {
      size_t idx = 0;
      port = std::stoi(port_str, &idx);
      if (idx != port_str.size() || port <= 0 || port > 65535) return nullptr;
    } catch (...) {
      return nullptr;
    }
    GstElement* s = gst_element_factory_make("udpsrc", "src");
    if (s) {
      GstCaps* caps =
          gst_caps_from_string("video/mpegts, systemstream=(boolean)true, packetsize=(int)188");
      g_object_set(s, "address", host.c_str(), "port", port, "caps", caps, "timeout",
                   kIdleTimeoutNs, nullptr);
      gst_caps_unref(caps);
      *udp = true;
    }
    return s;
  }
  if (scheme == "srt") {
    GstElement* s = gst_element_factory_make("srtsrc", "src");
    if (s) g_object_set(s, "uri", spec.c_str(), nullptr);
    return s;
  }
  const std::string path = (scheme == "file") ? spec.substr(colon + 1) : spec;
  GstElement* s = gst_element_factory_make("filesrc", "src");
  if (s) g_object_set(s, "location", path.c_str(), nullptr);
  return s;
}

}  // namespace

Result<std::monostate> extract(std::string_view source, const PacketHandler& on_packet,
                               std::stop_token stop, ExtractOptions options) {
  GstElement* pipeline = gst_pipeline_new("misbklv-extract");
  bool udp = false;
  GstElement* src = make_src(std::string(source), &udp);
  GstElement* demux = gst_element_factory_make("tsdemux", "demux");
  GstElement* sink = gst_element_factory_make("appsink", "sink");
  if (!pipeline || !src || !demux || !sink) {
    if (src) gst_object_unref(src);
    if (demux) gst_object_unref(demux);
    if (sink) gst_object_unref(sink);
    if (pipeline) gst_object_unref(pipeline);
    return Result<std::monostate>::err(Error::Backend);
  }
  g_object_set(sink, "emit-signals", TRUE, "sync", FALSE, nullptr);
  gst_bin_add_many(GST_BIN(pipeline), src, demux, sink, nullptr);
  gst_element_link(src, demux);

  ExtractCtx ctx(options.max_packet_bytes);
  ctx.sink = sink;
  ctx.on_packet = &on_packet;
  g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), &ctx);
  g_signal_connect(sink, "new-sample", G_CALLBACK(on_new_sample), &ctx);

  std::optional<Error> failure;
  bool cancelled = false;
  bool natural_end = false;
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    failure = Error::Backend;
  } else {
    GstBus* bus = gst_element_get_bus(pipeline);
    // Finite polling composes UDP idle/EOS/error handling with the stop token;
    // cancellation is cooperative and remains a successful termination.
    constexpr GstClockTime kPoll = 100 * GST_MSECOND;
    for (;;) {
      if (stop.stop_requested()) {
        cancelled = true;
        break;
      }
      if (ctx.has_frame_error.load(std::memory_order_acquire)) {
        failure = ctx.frame_error;
        break;
      }
      GstMessage* msg = gst_bus_timed_pop_filtered(
          bus, kPoll,
          static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_ELEMENT));
      if (!msg) continue;
      const GstMessageType t = GST_MESSAGE_TYPE(msg);
      bool done = false;
      if (t == GST_MESSAGE_EOS) {
        natural_end = true;
        done = true;
      }
      if (t == GST_MESSAGE_ERROR) {
        failure = Error::Backend;
        done = true;
      } else if (t == GST_MESSAGE_ELEMENT && udp) {
        const GstStructure* s = gst_message_get_structure(msg);
        if (s && gst_structure_has_name(s, "GstUDPSrcTimeout") &&
            ctx.received_bytes.load(std::memory_order_acquire)) {
          natural_end = true;
          done = true;
        }
      }
      gst_message_unref(msg);
      if (done) break;
    }
    gst_object_unref(bus);
  }
  // NULL joins the streaming thread, so the stack-owned callback context and
  // borrowed handler remain alive until every callback has returned.
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  if (cancelled || stop.stop_requested()) return Result<std::monostate>::ok({});
  if (ctx.has_frame_error.load(std::memory_order_acquire)) failure = ctx.frame_error;
  if (!failure && natural_end && !ctx.framer.remainder().empty()) {
    auto frame = inspect_packet_frame(ctx.framer.remainder(), options.max_packet_bytes);
    if (!frame)
      failure = frame.error();
    else if (!*frame)
      failure = Error::Truncated;
  }
  return failure ? Result<std::monostate>::err(*failure) : Result<std::monostate>::ok({});
}

}  // namespace misbklv::detail
