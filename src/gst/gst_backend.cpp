// SPDX-License-Identifier: Apache-2.0
// gstreamer MediaBackend (ADR 0013). Extraction: {file|udp|srt}src ! tsdemux !
// appsink; reassemble appsink fragments and frame whole KLV packets (B0 spike).
// Insertion (B2): appsrc ! mpegtsmux ! {file|udp|srt}sink. Live sources/sinks
// (udp/srt) add real-time pacing + idle-timeout termination (B4, ADR 0017).
#include "misbklv/gst_backend.hpp"

#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <gst/app/app.h>
#include <gst/gst.h>

#include "misbklv/packet.hpp"

namespace misbklv {
namespace {

// Reassembly + framing state, shared with the appsink callbacks (one streaming
// thread; extract() only blocks on the bus while these run).
struct ExtractCtx {
  GstElement* sink = nullptr;
  const PacketHandler* on_packet = nullptr;
  std::vector<std::byte> reassembly;
  std::size_t packets = 0;  // whole KLV packets emitted (gates the idle timeout)

  void drain() {
    std::size_t pos = 0;
    for (;;) {
      std::span<const std::byte> rest(reassembly.data() + pos, reassembly.size() - pos);
      const std::size_t n = packet_frame_length(rest);
      if (n == 0) break;  // need more data
      (*on_packet)(KlvPacket{rest.subspan(0, n), kNoPts});
      ++packets;
      pos += n;
    }
    if (pos) reassembly.erase(reassembly.begin(), reassembly.begin() + pos);
  }
};

void on_pad_added(GstElement*, GstPad* pad, gpointer user) {
  auto* ctx = static_cast<ExtractCtx*>(user);
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) caps = gst_pad_query_caps(pad, nullptr);
  const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  if (std::strcmp(name, "meta/x-klv") == 0) {
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
  GstBuffer* buf = gst_sample_get_buffer(sample);
  GstMapInfo mi;
  if (buf && gst_buffer_map(buf, &mi, GST_MAP_READ)) {
    const auto* p = reinterpret_cast<const std::byte*>(mi.data);
    ctx->reassembly.insert(ctx->reassembly.end(), p, p + mi.size);
    gst_buffer_unmap(buf, &mi);
    ctx->drain();
  }
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

// A live source that has delivered data but then goes quiet this long is treated
// as ended (no EOS crosses udp/srt). Longer than the ~33 ms inter-packet gap, so
// it never trips mid-stream; short enough to end the extract promptly (B4).
inline constexpr guint64 kIdleTimeoutNs = 500'000'000;  // 500 ms

// Build the source element from a spec: a bare path / "file:PATH" (EOS-ending),
// or a live "udp:HOST:PORT" / "srt:URI". Sets `*live` for the live schemes.
// Returns nullptr on a malformed spec.
GstElement* make_src(const std::string& spec, bool* live) {
  *live = false;
  const auto colon = spec.find(':');
  const std::string scheme =
      (colon == std::string::npos) ? std::string() : spec.substr(0, colon);
  if (scheme == "udp") {  // udp:HOST:PORT
    const std::string rest = spec.substr(colon + 1);
    const auto p = rest.rfind(':');
    if (p == std::string::npos) return nullptr;
    GstElement* s = gst_element_factory_make("udpsrc", "src");
    if (s) {
      GstCaps* caps = gst_caps_from_string(
          "video/mpegts, systemstream=(boolean)true, packetsize=(int)188");
      g_object_set(s, "address", rest.substr(0, p).c_str(), "port",
                   std::atoi(rest.substr(p + 1).c_str()), "caps", caps, "timeout",
                   kIdleTimeoutNs, nullptr);
      gst_caps_unref(caps);
      *live = true;
    }
    return s;
  }
  if (scheme == "srt") {
    GstElement* s = gst_element_factory_make("srtsrc", "src");
    if (s) g_object_set(s, "uri", spec.c_str(), nullptr);  // srt://...
    if (s) *live = true;
    return s;
  }
  const std::string path = (scheme == "file") ? spec.substr(colon + 1) : spec;
  GstElement* s = gst_element_factory_make("filesrc", "src");
  if (s) g_object_set(s, "location", path.c_str(), nullptr);
  return s;
}

// Build the sink element from an InsertConfig spec: "file:PATH" | "udp:HOST:PORT"
// | "srt:URI". Returns nullptr on an unknown scheme.
GstElement* make_sink(const std::string& spec) {
  const auto colon = spec.find(':');
  if (colon == std::string::npos) return nullptr;
  const std::string scheme = spec.substr(0, colon);
  const std::string rest = spec.substr(colon + 1);
  if (scheme == "file") {
    GstElement* s = gst_element_factory_make("filesink", "sink");
    if (s) g_object_set(s, "location", rest.c_str(), nullptr);
    return s;
  }
  if (scheme == "udp") {  // udp:HOST:PORT
    const auto p = rest.rfind(':');
    if (p == std::string::npos) return nullptr;
    GstElement* s = gst_element_factory_make("udpsink", "sink");
    if (s) g_object_set(s, "host", rest.substr(0, p).c_str(), "port",
                        std::atoi(rest.substr(p + 1).c_str()), nullptr);
    return s;
  }
  if (scheme == "srt") {
    GstElement* s = gst_element_factory_make("srtsink", "sink");
    if (s) g_object_set(s, "uri", spec.c_str(), nullptr);  // srt://...
    return s;
  }
  return nullptr;
}

// appsrc(meta/x-klv) ! mpegtsmux ! sink. push() blocks on appsrc backpressure;
// finish() sends EOS and drains. Stock mpegtsmux (gst >= 1.20) already emits
// stream_type 0x06 + KLVA, so no PMT rewrite is needed (fork 13; verified by
// round-trip).
class GstInserter : public Inserter {
 public:
  GstInserter(GstElement* pipeline, GstElement* appsrc)
      : pipeline_(pipeline), appsrc_(appsrc) {}
  ~GstInserter() override {
    if (pipeline_) {
      gst_element_set_state(pipeline_, GST_STATE_NULL);
      gst_object_unref(pipeline_);
    }
  }

  Result<std::monostate> push(std::span<const std::byte> pkt,
                              std::int64_t pts_ns) override {
    GstBuffer* buf = gst_buffer_new_allocate(nullptr, pkt.size(), nullptr);
    gst_buffer_fill(buf, 0, pkt.data(), pkt.size());
    GST_BUFFER_PTS(buf) = (pts_ns == kNoPts) ? pts_
                                             : static_cast<GstClockTime>(pts_ns);
    GST_BUFFER_DURATION(buf) = kFrameDur;
    pts_ += kFrameDur;
    const GstFlowReturn ret =
        gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf);  // consumes buf
    return ret == GST_FLOW_OK ? Result<std::monostate>::ok({})
                              : Result<std::monostate>::err(Error::Backend);
  }

  Result<std::monostate> finish() override {
    gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
    GstBus* bus = gst_element_get_bus(pipeline_);
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, GST_CLOCK_TIME_NONE,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = !(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR);
    if (msg) gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline_, GST_STATE_NULL);  // flush/close the sink
    return ok ? Result<std::monostate>::ok({})
              : Result<std::monostate>::err(Error::Backend);
  }

 private:
  static constexpr GstClockTime kFrameDur = 33'000'000;  // ~30 fps pacing
  GstElement* pipeline_;
  GstElement* appsrc_;
  GstClockTime pts_ = 0;
};

class GstBackend : public MediaBackend {
 public:
  Result<std::monostate> extract(std::string_view source,
                                 const PacketHandler& on_packet) override {
    gst_init(nullptr, nullptr);  // idempotent
    GstElement* pipeline = gst_pipeline_new("misbklv-extract");
    bool live = false;
    GstElement* src = make_src(std::string(source), &live);
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

    ExtractCtx ctx;
    ctx.sink = sink;
    ctx.on_packet = &on_packet;
    g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), &ctx);
    g_signal_connect(sink, "new-sample", G_CALLBACK(on_new_sample), &ctx);

    bool ok = true;
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      ok = false;
    } else {
      GstBus* bus = gst_element_get_bus(pipeline);
      // A file source ends at EOS; a live source never does, so udpsrc posts a
      // GstUDPSrcTimeout ELEMENT message once it idles — we treat that as end,
      // but only after data has flowed (an early timeout during startup, before
      // the sender is up, is ignored so it can't truncate the stream).
      for (;;) {
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus, GST_CLOCK_TIME_NONE,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR |
                                        GST_MESSAGE_ELEMENT));
        if (!msg) continue;
        const GstMessageType t = GST_MESSAGE_TYPE(msg);
        bool done = (t == GST_MESSAGE_EOS);
        if (t == GST_MESSAGE_ERROR) {
          ok = false;
          done = true;
        } else if (t == GST_MESSAGE_ELEMENT && live) {
          const GstStructure* s = gst_message_get_structure(msg);
          if (s && gst_structure_has_name(s, "GstUDPSrcTimeout") && ctx.packets > 0)
            done = true;
        }
        gst_message_unref(msg);
        if (done) break;
      }
      gst_object_unref(bus);
    }
    // set_state(NULL) blocks until streaming threads stop, so ctx (and the
    // borrowed callback) outlive all callbacks.
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return ok ? Result<std::monostate>::ok({})
              : Result<std::monostate>::err(Error::Backend);
  }

  Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig& cfg) override {
    gst_init(nullptr, nullptr);
    GstElement* pipeline = gst_pipeline_new("misbklv-insert");
    GstElement* appsrc = gst_element_factory_make("appsrc", "src");
    GstElement* mux = gst_element_factory_make("mpegtsmux", "mux");
    GstElement* sink = make_sink(cfg.sink);
    if (!pipeline || !appsrc || !mux || !sink) {
      if (sink) gst_object_unref(sink);
      if (mux) gst_object_unref(mux);
      if (appsrc) gst_object_unref(appsrc);
      if (pipeline) gst_object_unref(pipeline);
      return Result<std::unique_ptr<Inserter>>::err(sink ? Error::Backend
                                                         : Error::Unsupported);
    }
    GstCaps* caps = gst_caps_from_string("meta/x-klv, parsed=(boolean)true");
    // realtime: the appsrc is a live source and the sink renders on the clock, so
    // output is paced to the per-buffer PTS (~30 fps) instead of pushed as fast as
    // the sink drains — the real-time streaming behavior (B4). Off = fast, for a
    // filesink round-trip.
    g_object_set(appsrc, "caps", caps, "format", GST_FORMAT_TIME, "block", TRUE,
                 "is-live", cfg.realtime ? TRUE : FALSE, nullptr);
    gst_caps_unref(caps);
    if (cfg.realtime) g_object_set(sink, "sync", TRUE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), appsrc, mux, sink, nullptr);
    if (!gst_element_link_many(appsrc, mux, sink, nullptr) ||
        gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE) {
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      return Result<std::unique_ptr<Inserter>>::err(Error::Backend);
    }
    return Result<std::unique_ptr<Inserter>>::ok(
        std::make_unique<GstInserter>(pipeline, appsrc));
  }
};

}  // namespace

std::unique_ptr<MediaBackend> make_gst_backend() {
  return std::make_unique<GstBackend>();
}

}  // namespace misbklv
