// SPDX-License-Identifier: Apache-2.0
// GStreamer insertion: sink/appsrc pipeline ownership, KLV pushes, video-branch
// coordination, finish/drain, and output-file cleanup.
#include "gst_backend_internal.hpp"

#include <cstdlib>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include <gst/app/app.h>

namespace misbklv::detail {
namespace {

// A stall guard, not a performance budget: finish may need to remux the rest of
// a video file after the caller's final KLV packet.
inline constexpr GstClockTime kFinishDrainTimeout = 300 * GST_SECOND;

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
  if (scheme == "udp") {
    const auto p = rest.rfind(':');
    if (p == std::string::npos) return nullptr;
    GstElement* s = gst_element_factory_make("udpsink", "sink");
    if (s) g_object_set(s, "host", rest.substr(0, p).c_str(), "port",
                        std::atoi(rest.substr(p + 1).c_str()), nullptr);
    return s;
  }
  if (scheme == "srt") {
    GstElement* s = gst_element_factory_make("srtsink", "sink");
    if (s) g_object_set(s, "uri", spec.c_str(), nullptr);
    return s;
  }
  return nullptr;
}

// appsrc(meta/x-klv) ! mpegtsmux ! sink. push() observes appsrc backpressure;
// finish() sends EOS and waits for the whole pipeline to drain.
class GstInserter : public Inserter {
 public:
  // `removable_sink` is the sink's file path iff this session created it and
  // may delete it again. It stays empty for a non-file sink and for a caller's
  // pre-existing path (ADR 0022).
  GstInserter(GstElement* pipeline, GstElement* appsrc,
              std::unique_ptr<VideoCtx> video = nullptr,
              std::string removable_sink = {})
      : pipeline_(pipeline), appsrc_(appsrc), video_(std::move(video)),
        removable_sink_(std::move(removable_sink)) {}

  ~GstInserter() override {
    if (pipeline_) {
      // NULL joins streaming threads before video_ is destroyed, so pad-added
      // callbacks and the SEI probe never observe a dangling user pointer.
      gst_element_set_state(pipeline_, GST_STATE_NULL);
      gst_object_unref(pipeline_);
    }
    // An abandoned session has not produced output, only an unfinalized file.
    // Same as a failed finish(): no error path leaves a newly-created file.
    discard_output();
  }

  Result<std::monostate> push(std::span<const std::byte> pkt,
                              std::int64_t pts_ns) override {
    // With video passthrough both branches must share the source timeline. The
    // synthetic ~30fps KLV-only counter is therefore a caller error here.
    if (video_ && pts_ns == kNoPts)
      return Result<std::monostate>::err(Error::Unsupported);
    // Generate mode records the ST 0601 Item 2 sensor timestamp against this
    // KLV PTS; the video pad probe consumes that mapping later.
    if (video_ && video_->generate_sei && pts_ns != kNoPts)
      record_sensor_timestamp(*video_, pkt, pts_ns);

    GstBuffer* buf = gst_buffer_new_allocate(nullptr, pkt.size(), nullptr);
    gst_buffer_fill(buf, 0, pkt.data(), pkt.size());
    // KLV-only output with kNoPts retains the historic ~30fps pacing counter.
    GST_BUFFER_PTS(buf) = (pts_ns == kNoPts) ? pts_
                                             : static_cast<GstClockTime>(pts_ns);
    GST_BUFFER_DURATION(buf) = kFrameDur;
    pts_ += kFrameDur;
    const GstFlowReturn ret =
        gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf);
    return ret == GST_FLOW_OK ? Result<std::monostate>::ok({})
                              : Result<std::monostate>::err(Error::Backend);
  }

  Result<std::monostate> finish() override {
    gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
    GstBus* bus = gst_element_get_bus(pipeline_);
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, kFinishDrainTimeout,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    // Only a clean EOS succeeds. The five-minute bound is a stall guard for a
    // video branch that never drains, not a normal performance deadline.
    const bool ok = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (!msg)
      g_warning("misbklv: pipeline did not drain within %" G_GUINT64_FORMAT
                "s of EOS; giving up",
                static_cast<guint64>(kFinishDrainTimeout / GST_SECOND));
    if (msg) gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (ok)
      removable_sink_.clear();
    else
      discard_output();
    return ok ? Result<std::monostate>::ok({})
              : Result<std::monostate>::err(Error::Backend);
  }

 private:
  void discard_output() {
    // Called after NULL, when filesink has closed the path.
    if (removable_sink_.empty()) return;
    std::remove(removable_sink_.c_str());
    removable_sink_.clear();
  }

  static constexpr GstClockTime kFrameDur = 33'000'000;
  GstElement* pipeline_;
  GstElement* appsrc_;
  std::unique_ptr<VideoCtx> video_;
  GstClockTime pts_ = 0;
  std::string removable_sink_;
};

}  // namespace

Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig& cfg) {
  using R = Result<std::unique_ptr<Inserter>>;
  std::string video_path;
  if (!cfg.video_source.empty()) {
    // Validate before building: filesink creates a file as soon as the pipeline
    // leaves NULL, and a failed open must not leave a partial output behind.
    if (cfg.realtime) return R::err(Error::Unsupported);
    video_path = cfg.video_source;
    if (video_path.rfind("file:", 0) == 0) video_path.erase(0, 5);
    std::FILE* f = std::fopen(video_path.c_str(), "rb");
    if (!f) return R::err(Error::Backend);
    std::fclose(f);
  }
  std::string sink_path;
  bool sink_preexisted = true;
  if (cfg.sink.rfind("file:", 0) == 0) {
    // Keep enough ownership state to remove only files this session created.
    sink_path = cfg.sink.substr(5);
    std::FILE* f = std::fopen(sink_path.c_str(), "rb");
    sink_preexisted = (f != nullptr);
    if (f) std::fclose(f);
  }
  auto fail = [&](GstElement* pipe, Error e) {
    // Teardown must precede destruction of a prepared VideoCtx: its callbacks
    // are attached to elements in this pipeline.
    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(pipe);
    if (!sink_path.empty() && !sink_preexisted) std::remove(sink_path.c_str());
    return R::err(e);
  };

  GstElement* pipeline = gst_pipeline_new("misbklv-insert");
  GstElement* appsrc = gst_element_factory_make("appsrc", "src");
  GstElement* mux = gst_element_factory_make("mpegtsmux", "mux");
  GstElement* sink = make_sink(cfg.sink);
  if (!pipeline || !appsrc || !mux || !sink) {
    if (sink) gst_object_unref(sink);
    if (mux) gst_object_unref(mux);
    if (appsrc) gst_object_unref(appsrc);
    if (pipeline) gst_object_unref(pipeline);
    return R::err(sink ? Error::Backend : Error::Unsupported);
  }
  GstCaps* caps = gst_caps_from_string("meta/x-klv, parsed=(boolean)true");
  // realtime makes appsrc live and lets the sink render on the clock, pacing
  // UDP/SRT output to its per-buffer PTS instead of pushing as fast as possible.
  g_object_set(appsrc, "caps", caps, "format", GST_FORMAT_TIME, "block", TRUE,
               "is-live", cfg.realtime ? TRUE : FALSE, nullptr);
  gst_caps_unref(caps);
  if (cfg.realtime) g_object_set(sink, "sync", TRUE, nullptr);
  gst_bin_add_many(GST_BIN(pipeline), appsrc, mux, sink, nullptr);
  // Reserve the video muxer pad FIRST, while the pipeline is still NULL, so it
  // takes the lower ES PID. mpegtsmux orders the PMT by ES PID and allocates
  // them in pad-request order, so video must be requested before the KLV
  // appsrc links (which auto-requests the next pad). The pad stays unlinked
  // until the demuxer exposes its video pad during PAUSED preroll; reserving
  // it here fixes its PMT position without deferring the KLV link at all
  // (ADR 0020 § stream order — the deferral that previously destabilized
  // ST 0604 SEI timing). Borrowed after the unref: the muxer owns the pad.
  GstPad* reserved_video_pad = nullptr;
  if (!video_path.empty()) {
    reserved_video_pad = gst_element_request_pad_simple(mux, "sink_%d");
    if (!reserved_video_pad) return fail(pipeline, Error::Backend);
    gst_object_unref(reserved_video_pad);
    // Link the KLV appsrc onto an explicitly-requested pad one above the
    // video pad's. gst_element_link would re-request "sink_%d" and grab the
    // reserved video pad instead of allocating the next free one, handing the
    // metadata stream the video pad's PMT position.
    GstPadTemplate* tpl =
        gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(mux), "sink_%d");
    const gchar* vname = GST_PAD_NAME(reserved_video_pad);
    gchar* kname = g_strdup_printf("sink_%d", std::atoi(vname + 5) + 1);
    GstPad* klv_pad = gst_element_request_pad(mux, tpl, kname, nullptr);
    g_free(kname);
    if (!klv_pad) return fail(pipeline, Error::Backend);
    GstPad* srcpad = gst_element_get_static_pad(appsrc, "src");
    const bool klv_linked =
        srcpad && gst_pad_link(srcpad, klv_pad) == GST_PAD_LINK_OK;
    if (srcpad) gst_object_unref(srcpad);
    gst_object_unref(klv_pad);
    if (!klv_linked || !gst_element_link(mux, sink))
      return fail(pipeline, Error::Backend);
  } else if (!gst_element_link_many(appsrc, mux, sink, nullptr)) {
    return fail(pipeline, Error::Backend);
  }

  std::unique_ptr<VideoCtx> video;
  if (!video_path.empty()) {
    // Dynamic pads are linked by the video unit through parsers where required;
    // nothing is decoded, preserving codec passthrough.
    auto prepared =
        prepare_video_branch(pipeline, mux, reserved_video_pad, video_path,
                             cfg.sei_0604, video);
    if (!prepared) return fail(pipeline, prepared.error());
  }
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    return fail(pipeline, Error::Backend);
  return R::ok(std::make_unique<GstInserter>(
      pipeline, appsrc, std::move(video),
      sink_preexisted ? std::string() : sink_path));
}

}  // namespace misbklv::detail
