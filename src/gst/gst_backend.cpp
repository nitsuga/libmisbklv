// SPDX-License-Identifier: Apache-2.0
// gstreamer MediaBackend (ADR 0013). Extraction: filesrc ! tsdemux ! appsink;
// reassemble appsink fragments and frame whole KLV packets (B0 spike). Insertion
// is B2 (returns Unsupported for now).
#include "misbklv/gst_backend.hpp"

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

  void drain() {
    std::size_t pos = 0;
    for (;;) {
      std::span<const std::byte> rest(reassembly.data() + pos, reassembly.size() - pos);
      const std::size_t n = packet_frame_length(rest);
      if (n == 0) break;  // need more data
      (*on_packet)(KlvPacket{rest.subspan(0, n), kNoPts});
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

class GstBackend : public MediaBackend {
 public:
  Result<std::monostate> extract(std::string_view source,
                                 const PacketHandler& on_packet) override {
    gst_init(nullptr, nullptr);  // idempotent
    GstElement* pipeline = gst_pipeline_new("misbklv-extract");
    GstElement* src = gst_element_factory_make("filesrc", "src");
    GstElement* demux = gst_element_factory_make("tsdemux", "demux");
    GstElement* sink = gst_element_factory_make("appsink", "sink");
    if (!pipeline || !src || !demux || !sink) {
      if (pipeline) gst_object_unref(pipeline);
      return Result<std::monostate>::err(Error::Backend);
    }
    const std::string loc(source);
    g_object_set(src, "location", loc.c_str(), nullptr);
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
      GstMessage* msg = gst_bus_timed_pop_filtered(
          bus, GST_CLOCK_TIME_NONE,
          static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
      if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) ok = false;
        gst_message_unref(msg);
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

  Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig&) override {
    return Result<std::unique_ptr<Inserter>>::err(Error::Unsupported);  // B2
  }
};

}  // namespace

std::unique_ptr<MediaBackend> make_gst_backend() {
  return std::make_unique<GstBackend>();
}

}  // namespace misbklv
