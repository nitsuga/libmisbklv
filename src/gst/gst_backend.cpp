// SPDX-License-Identifier: Apache-2.0
// gstreamer MediaBackend (ADR 0013). Extraction: {file|udp|srt}src ! tsdemux !
// appsink; reassemble appsink fragments and frame whole KLV packets (B0 spike).
// Insertion (B2): appsrc ! mpegtsmux ! {file|udp|srt}sink, optionally joined by
// a video passthrough branch filesrc ! parsebin (ADR 0020). Live sources/sinks
// (udp/srt) add real-time pacing + idle-timeout termination (B4, ADR 0017).
#include "misbklv/gst_backend.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <gst/app/app.h>
#include <gst/gst.h>

#include "misbklv/packet.hpp"
#include "../pts_marks.hpp"

namespace misbklv {
namespace {

// Reassembly + framing state, shared with the appsink callbacks (one streaming
// thread; extract() only blocks on the bus while these run).
struct ExtractCtx {
  GstElement* sink = nullptr;
  const PacketHandler* on_packet = nullptr;
  std::vector<std::byte> reassembly;
  std::size_t packets = 0;  // whole KLV packets emitted (gates the idle timeout)
  detail::PtsMarks marks;   // buffer timestamps -> per-packet timestamps
  std::size_t stream_off = 0;  // absolute offset of reassembly[0] in the stream

  void drain() {
    std::size_t pos = 0;
    for (;;) {
      std::span<const std::byte> rest(reassembly.data() + pos, reassembly.size() - pos);
      const std::size_t n = packet_frame_length(rest);
      if (n == 0) break;  // need more data
      (*on_packet)(KlvPacket{rest.subspan(0, n), marks.at(stream_off + pos)});
      ++packets;
      pos += n;
    }
    if (pos) {
      reassembly.erase(reassembly.begin(), reassembly.begin() + pos);
      stream_off += pos;
    }
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
    // Timestamp these bytes before they lose their identity in the reassembly
    // buffer (ADR 0021). The *running* time, not the raw buffer PTS: tsdemux's
    // segment is program-wide and starts at the earliest timestamp in the
    // program, so running time is nanoseconds from the start of the source —
    // the same timeline push() writes on, which is what makes read -> edit ->
    // write compose. An untimed buffer marks kNoPts.
    std::int64_t pts_ns = kNoPts;
    const GstSegment* seg = gst_sample_get_segment(sample);
    if (seg && GST_BUFFER_PTS_IS_VALID(buf)) {
      const GstClockTime rt =
          gst_segment_to_running_time(seg, GST_FORMAT_TIME, GST_BUFFER_PTS(buf));
      if (GST_CLOCK_TIME_IS_VALID(rt)) pts_ns = static_cast<std::int64_t>(rt);
    }
    ctx->marks.mark(ctx->stream_off + ctx->reassembly.size(), pts_ns);
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

// --- video passthrough branch (ADR 0020) ------------------------------------
// State shared with parsebin's dynamic-pad callbacks. parsebin exposes one pad
// per elementary stream once it has parsed the container; we link the FIRST
// video/* pad to a mpegtsmux request pad and ignore every other pad (audio,
// subtitles, and any KLV the source already carries — the caller supplies its
// own). open_insert() waits on `cv` for that pad, so the muxer never writes a
// PMT before the video stream has joined. Lives as long as the pipeline.
//
// Bound on that wait: parsing a container header is fast (milliseconds), so this
// only ever fires on a source that never yields a pad at all.
inline constexpr std::chrono::seconds kVideoPadTimeout{10};

struct VideoCtx {
  GstElement* mux = nullptr;
  GstElement* pipeline = nullptr;  // for creating fakesinks
  std::mutex mu;
  std::condition_variable cv;
  bool linked = false;         // a video pad reached the muxer
  bool no_more_pads = false;   // parsebin is done exposing pads
  int ignored_video_pads = 0;  // 2nd+ video stream: carried? no. recorded? yes.
};

// True for caps whose media type is video/* (video/x-h264, video/x-h265, ...).
bool caps_are_video(GstCaps* caps) {
  if (!caps || gst_caps_is_empty(caps)) return false;
  const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  return name && std::strncmp(name, "video/", 6) == 0;
}

void on_video_pad_added(GstElement*, GstPad* pad, gpointer user) {
  auto* ctx = static_cast<VideoCtx*>(user);
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) caps = gst_pad_query_caps(pad, nullptr);
  const bool video = caps_are_video(caps);
  if (caps) gst_caps_unref(caps);

  if (!video) {
    // audio / subtitles / source-side KLV: dropped, but parsebin requires all pads
    // to be linked. Link to fakesink to satisfy parsebin and prevent "not-linked" errors.
    GstElement* fakesink = gst_element_factory_make("fakesink", nullptr);
    if (fakesink) {
      gst_bin_add(GST_BIN(ctx->pipeline), fakesink);
      gst_element_sync_state_with_parent(fakesink);  // sync state before linking
      GstPad* sinkpad = gst_element_get_static_pad(fakesink, "sink");
      if (sinkpad) {
        GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
        if (ret != GST_PAD_LINK_OK) {
          g_warning("misbklv: failed to link non-video pad to fakesink: %d", ret);
        }
        gst_object_unref(sinkpad);
      }
    }
    return;
  }

  std::lock_guard<std::mutex> lk(ctx->mu);
  if (ctx->linked) {  // a second video stream: carry the first, note this one
    ++ctx->ignored_video_pads;
    return;
  }

  // parsebin outputs H.264 in avc format (with codec_data), but mpegtsmux needs
  // byte-stream format. Insert h264parse to convert the format.
  GstElement* parse = gst_element_factory_make("h264parse", nullptr);
  if (!parse) {
    g_warning("misbklv: failed to create h264parse element");
    ctx->cv.notify_all();
    return;
  }

  // config-interval=-1: insert SPS/PPS with every IDR frame, which helps maintain
  // parameter set availability for downstream readers. Note: ST 0604 Picture Timing
  // SEI messages (present in sources like Parrot MP4s) may still generate warnings
  // in some readers due to VUI/SPS association complexities in the remux path — full
  // ST 0604 support is deferred (ADR 0009).
  g_object_set(parse, "config-interval", -1, nullptr);

  gst_bin_add(GST_BIN(ctx->pipeline), parse);
  gst_element_sync_state_with_parent(parse);

  // Link: parsebin pad -> h264parse -> muxer
  GstPad* parse_sink = gst_element_get_static_pad(parse, "sink");
  if (!parse_sink) {
    g_warning("misbklv: failed to get h264parse sink pad");
    ctx->cv.notify_all();
    return;
  }

  GstPadLinkReturn ret = gst_pad_link(pad, parse_sink);
  gst_object_unref(parse_sink);

  if (ret != GST_PAD_LINK_OK) {
    g_warning("misbklv: failed to link video pad to h264parse: %d (parsebin parent: %s, h264parse parent: %s)",
              ret,
              GST_ELEMENT_NAME(gst_pad_get_parent_element(pad)),
              GST_ELEMENT_NAME(GST_ELEMENT_PARENT(parse)));
    ctx->cv.notify_all();
    return;
  }

  // Link h264parse to muxer using gst_element_link (simpler and handles pad requests)
  if (gst_element_link(parse, ctx->mux)) {
    ctx->linked = true;
  } else {
    g_warning("misbklv: failed to link h264parse to muxer");
  }
  ctx->cv.notify_all();  // wake open_insert (linked, or a failure it will time out on)
}

void on_video_no_more_pads(GstElement*, gpointer user) {
  auto* ctx = static_cast<VideoCtx*>(user);
  {
    std::lock_guard<std::mutex> lk(ctx->mu);
    ctx->no_more_pads = true;
  }
  ctx->cv.notify_all();
}

// appsrc(meta/x-klv) ! mpegtsmux ! sink. push() blocks on appsrc backpressure;
// finish() sends EOS and drains. Stock mpegtsmux (gst >= 1.20) already emits
// stream_type 0x06 + KLVA, so no PMT rewrite is needed (fork 13; verified by
// round-trip).
class GstInserter : public Inserter {
 public:
  // `removable_sink` is the sink's file path IF this session created it and may
  // therefore delete it again — empty for a non-file sink, or for a path that
  // already existed (that file is the caller's, ADR 0022).
  GstInserter(GstElement* pipeline, GstElement* appsrc,
              std::unique_ptr<VideoCtx> video = nullptr,
              std::string removable_sink = {})
      : pipeline_(pipeline), appsrc_(appsrc), video_(std::move(video)),
        removable_sink_(std::move(removable_sink)) {}
  ~GstInserter() override {
    if (pipeline_) {
      gst_element_set_state(pipeline_, GST_STATE_NULL);  // joins the streaming
      gst_object_unref(pipeline_);  // threads, so video_ outlives its callbacks
    }
    // An abandoned session — destroyed without a finish() that returned ok — has
    // not produced output, only an unfinalized file. Same guarantee as a failed
    // finish(): no error path leaves a file behind (ADR 0022).
    discard_output();
  }

  Result<std::monostate> push(std::span<const std::byte> pkt,
                              std::int64_t pts_ns) override {
    // With a video branch both streams must share the source's timeline, so the
    // synthesized ~30 fps counter is a caller error, not a fallback (ADR 0020).
    if (video_ && pts_ns == kNoPts)
      return Result<std::monostate>::err(Error::Unsupported);
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
    if (ok)
      removable_sink_.clear();  // this is the output now; never delete it
    else
      discard_output();  // an error result leaves no output file (ADR 0022)
    return ok ? Result<std::monostate>::ok({})
              : Result<std::monostate>::err(Error::Backend);
  }

 private:
  // Remove the sink file this session created, if it still owns one. Called
  // after the pipeline is in NULL, so the sink has closed the file first.
  void discard_output() {
    if (removable_sink_.empty()) return;
    std::remove(removable_sink_.c_str());
    removable_sink_.clear();
  }

  static constexpr GstClockTime kFrameDur = 33'000'000;  // ~30 fps pacing
  GstElement* pipeline_;
  GstElement* appsrc_;
  std::unique_ptr<VideoCtx> video_;  // null = KLV-only pipeline
  GstClockTime pts_ = 0;
  std::string removable_sink_;  // empty once there is nothing we may delete
};

class GstBackend : public MediaBackend {
 public:
  Result<std::monostate> extract(std::string_view source,
                                 const PacketHandler& on_packet,
                                 std::stop_token stop = {}) override {
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
      // the sender is up, is ignored so it can't truncate the stream). A finite
      // pop timeout lets us also poll `stop` for cooperative early cancel (ADR
      // 0019) — the exit path for a KlvStream consumer that breaks.
      constexpr GstClockTime kPoll = 100 * GST_MSECOND;
      for (;;) {
        if (stop.stop_requested()) break;  // cancelled (ok stays true)
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus, kPoll,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR |
                                        GST_MESSAGE_ELEMENT));
        if (!msg) continue;  // timeout -> re-check stop / keep waiting
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
    using R = Result<std::unique_ptr<Inserter>>;
    gst_init(nullptr, nullptr);
    // Validate the video source BEFORE anything is built: a filesink creates its
    // file the moment the pipeline leaves NULL, and a failed open_insert must not
    // leave a partial output behind (ADR 0020).
    std::string video_path;
    if (!cfg.video_source.empty()) {
      if (cfg.realtime) return R::err(Error::Unsupported);  // untested pairing
      video_path = cfg.video_source;
      if (video_path.rfind("file:", 0) == 0) video_path.erase(0, 5);
      std::FILE* f = std::fopen(video_path.c_str(), "rb");
      if (!f) return R::err(Error::Backend);  // missing / unreadable source
      std::fclose(f);
    }
    // Not every failure can be caught before building: a videoless-but-readable
    // source only reveals itself in PAUSED, by which point a file sink has
    // already created its file. A leftover zero-byte .ts reads as output to
    // anything scanning the directory and silently clobbers whatever was at that
    // path, so failures below unlink it — but only if THIS call created it, so a
    // pre-existing file the caller cared about is never deleted by a failed open.
    // The same path+flag is handed to the Inserter, which extends the guarantee
    // over the rest of the session (a failing finish(), or an abandoned one —
    // ADR 0022).
    std::string sink_path;
    bool sink_preexisted = true;
    if (cfg.sink.rfind("file:", 0) == 0) {
      sink_path = cfg.sink.substr(5);
      std::FILE* f = std::fopen(sink_path.c_str(), "rb");
      sink_preexisted = (f != nullptr);
      if (f) std::fclose(f);
    }
    // Teardown for every failure past pipeline construction. set_state(NULL)
    // closes the sink's file before we unlink it.
    auto fail = [&](GstElement* pipe, Error e) {
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
    if (!gst_element_link_many(appsrc, mux, sink, nullptr))
      return fail(pipeline, Error::Backend);

    // Video passthrough: filesrc ! parsebin, joining the same muxer. parsebin
    // auto-plugs demuxer + parser for whatever the container holds and never
    // decodes, which is what keeps this codec-agnostic (H.264 / H.265 alike).
    std::unique_ptr<VideoCtx> video;
    if (!video_path.empty()) {
      GstElement* vsrc = gst_element_factory_make("filesrc", "vsrc");
      GstElement* parse = gst_element_factory_make("parsebin", "vparse");
      if (!vsrc || !parse) {
        if (vsrc) gst_object_unref(vsrc);
        if (parse) gst_object_unref(parse);
        return fail(pipeline, Error::Backend);
      }
      g_object_set(vsrc, "location", video_path.c_str(), nullptr);
      video = std::make_unique<VideoCtx>();
      video->mux = mux;
      video->pipeline = pipeline;  // for creating fakesinks in pad callback
      g_signal_connect(parse, "pad-added", G_CALLBACK(on_video_pad_added),
                       video.get());
      g_signal_connect(parse, "no-more-pads", G_CALLBACK(on_video_no_more_pads),
                       video.get());
      gst_bin_add_many(GST_BIN(pipeline), vsrc, parse, nullptr);
      if (!gst_element_link(vsrc, parse)) return fail(pipeline, Error::Backend);
    }

    // parsebin exposes its pads while prerolling in PAUSED. Wait for the video
    // pad here, before PLAYING, so the muxer's PMT is written with both
    // elementary streams — otherwise a caller pushing KLV immediately could race
    // the video pad and produce a KLV-only PMT. (KLV-only keeps going straight
    // to PLAYING, exactly as before.)
    if (video) {
      if (gst_element_set_state(pipeline, GST_STATE_PAUSED) ==
          GST_STATE_CHANGE_FAILURE)
        return fail(pipeline, Error::Backend);
      GstBus* bus = gst_element_get_bus(pipeline);
      const auto deadline = std::chrono::steady_clock::now() + kVideoPadTimeout;
      bool linked = false;
      int ignored = 0;
      for (;;) {
        {  // woken by pad-added / no-more-pads; short waits so we also see errors
          std::unique_lock<std::mutex> lk(video->mu);
          if (video->cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return video->linked || video->no_more_pads;
              })) {
            linked = video->linked;
            ignored = video->ignored_video_pads;
            break;
          }
        }
        GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (msg) {  // not a media file / no demuxer for it
          // The caller only ever sees Error::Unsupported, so surface the detail
          // here rather than dropping it silently (it is also consumed from the
          // bus, so finish() could not report it later).
          GError* err = nullptr;
          gchar* dbg = nullptr;
          gst_message_parse_error(msg, &err, &dbg);
          // Check if this is a "not-linked" error from qtdemux - this can happen
          // transiently during pad linking but doesn't mean the video source is invalid
          const bool is_not_linked = (dbg && std::strstr(dbg, "not-linked"));
          if (!is_not_linked) {
            g_warning("misbklv: video source '%s': %s", video_path.c_str(),
                      err ? err->message : "pipeline error");
            if (err) g_error_free(err);
            g_free(dbg);
            gst_message_unref(msg);
            break;
          }
          // Ignore transient not-linked errors - wait for linked status instead
          if (err) g_error_free(err);
          g_free(dbg);
          gst_message_unref(msg);
        }
        if (std::chrono::steady_clock::now() > deadline) break;
      }
      gst_object_unref(bus);
      if (!linked)  // unparseable source, or no video stream in it
        return fail(pipeline, Error::Unsupported);
      if (ignored)
        g_warning("misbklv: %d extra video stream(s) in '%s' not carried",
                  ignored, video_path.c_str());
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE)
      return fail(pipeline, Error::Backend);
    return R::ok(std::make_unique<GstInserter>(
        pipeline, appsrc, std::move(video),
        sink_preexisted ? std::string() : sink_path));
  }
};

}  // namespace

std::unique_ptr<MediaBackend> make_gst_backend() {
  return std::make_unique<GstBackend>();
}

}  // namespace misbklv
