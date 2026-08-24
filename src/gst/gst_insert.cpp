// SPDX-License-Identifier: Apache-2.0
// GStreamer insertion: sink/appsrc pipeline ownership, KLV pushes, video-branch
// coordination, finish/drain, and output-file cleanup.
#include "gst_backend_internal.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <gst/app/app.h>

namespace misbklv::detail {
namespace {

// A stall guard, not a performance budget: finish may need to remux the rest of
// a video file after the caller's final KLV packet.
inline constexpr GstClockTime kFinishDrainTimeout = 300 * GST_SECOND;

// How long each drain poll blocks before the loop re-checks its stop token and
// deadline. Short enough that a cancellation takes effect promptly (the whole
// point of threading a stop_token in — ADR 0032), long enough not to spin.
inline constexpr GstClockTime kFinishDrainPoll = 100 * GST_MSECOND;

// NULL teardown should normally complete synchronously, but a misbehaving
// source/plugin must not turn close() or destruction into an unbounded wait.
inline constexpr GstClockTime kNullStateTimeout = 5 * GST_SECOND;

// A caller can close immediately after opening a live source. Give its final
// src pad a short, bounded window to negotiate caps before sending EOS; without
// caps mpegtsmux cannot create the video stream handler.
inline constexpr auto kLiveEosReadyTimeout = std::chrono::seconds(1);

// EOS is serialized with video data and would otherwise wait indefinitely for
// the mux sink pad's stream lock. Poll that lock so cancellation and a stuck
// streaming thread remain bounded before entering the event handler.
inline constexpr auto kLiveEosLockTimeout = std::chrono::seconds(5);

}  // namespace

bool push_live_eos_when_idle(
    GstPad* source_pad, GstPad* mux_pad, std::stop_token stop,
    std::chrono::steady_clock::duration lock_timeout) {
  if (!source_pad || !mux_pad) return false;
  const auto deadline = std::chrono::steady_clock::now() + lock_timeout;
  bool sent = false;
  while (!stop.stop_requested() &&
         std::chrono::steady_clock::now() < deadline) {
    // Acquire in the event's source-to-sink order. Checking only source_pad is
    // insufficient: a branch ghost-src lock can remain free while a mux chain
    // function holds the downstream lock. Both are recursive, so the event can
    // safely re-enter them on this thread.
    if (GST_PAD_STREAM_TRYLOCK(source_pad)) {
      if (GST_PAD_STREAM_TRYLOCK(mux_pad)) {
        sent = gst_pad_push_event(source_pad, gst_event_new_eos());
        GST_PAD_STREAM_UNLOCK(mux_pad);
        GST_PAD_STREAM_UNLOCK(source_pad);
        break;
      }
      GST_PAD_STREAM_UNLOCK(source_pad);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return sent;
}

// NOTE: make_sink intentionally lives at misbklv::detail scope (not in the
// anonymous namespace below) so the unit tests can construct and inspect a sink.

GstElement* make_sink(const std::string& spec, const InsertConfig& cfg) {
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
    // Validate multicast TTL before creating the element so an invalid TTL
    // cleanly returns an insertion error even when the udpsink factory is
    // unavailable (s == nullptr) — otherwise gst_object_unref(nullptr) would
    // emit a GLib critical.
    if (cfg.udp_ttl_mcast < 0 || cfg.udp_ttl_mcast > 255) return nullptr;
    GstElement* s = gst_element_factory_make("udpsink", "sink");
    if (!s) return nullptr;
    g_object_set(s, "host", host.c_str(), "port", port, nullptr);
    g_object_set(s, "auto-multicast", TRUE, "ttl-mc", cfg.udp_ttl_mcast,
                 "loop", cfg.udp_loop ? TRUE : FALSE, nullptr);
    // Only pin the egress interface when asked; "" keeps the stack default.
    // udpsink's multicast-iface is null by default, and GStreamer treats "" as
    // a set-but-empty value, so leave it unset unless the caller gave a name.
    if (!cfg.udp_mcast_iface.empty())
      g_object_set(s, "multicast-iface", cfg.udp_mcast_iface.c_str(), nullptr);
    return s;
  }
  if (scheme == "srt") {
    GstElement* s = gst_element_factory_make("srtsink", "sink");
    if (s) g_object_set(s, "uri", spec.c_str(), nullptr);
    return s;
  }
  return nullptr;
}

namespace {

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
      quiesce_to_null();
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

  Result<std::monostate> push(std::vector<std::byte>&& pkt,
                              std::int64_t pts_ns) override {
    if (video_ && pts_ns == kNoPts)
      return Result<std::monostate>::err(Error::Unsupported);
    if (video_ && video_->generate_sei && pts_ns != kNoPts)
      record_sensor_timestamp(*video_,
                              std::span<const std::byte>(pkt.data(), pkt.size()),
                              pts_ns);
    GstBuffer* buf = nullptr;
    if (pkt.empty()) {
      buf = gst_buffer_new();
    } else {
      // Transfer ownership of the vector's storage to GStreamer without copying.
      auto* vec = new std::vector<std::byte>(std::move(pkt));
      buf = gst_buffer_new_wrapped_full(
          static_cast<GstMemoryFlags>(0), vec->data(), vec->size(), 0,
          vec->size(), vec, [](gpointer data) {
            delete static_cast<std::vector<std::byte>*>(data);
          });
    }
    GST_BUFFER_PTS(buf) = (pts_ns == kNoPts) ? pts_
                                              : static_cast<GstClockTime>(pts_ns);
    GST_BUFFER_DURATION(buf) = kFrameDur;
    pts_ += kFrameDur;
    const GstFlowReturn ret =
        gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf);
    return ret == GST_FLOW_OK ? Result<std::monostate>::ok({})
                              : Result<std::monostate>::err(Error::Backend);
  }

  // An unbounded live video source will never produce EOS by itself. Send EOS
  // from its final src pad while it remains linked to mpegtsmux, then wait for
  // the muxer/sink to drain both the video and KLV pads. In particular, do not
  // unlink or release the mux request pad while PLAYING: mpegtsmux can still be
  // traversing its request-pad list on a streaming thread (issue #39).
  Result<std::monostate> finish(std::stop_token stop) override {
    const GstFlowReturn klv_eos =
        gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
    bool live_video_eos = true;
    if (video_ && video_->is_live_unbounded && video_->reserved_video_pad) {
      GstPad* peer = gst_pad_get_peer(video_->reserved_video_pad);
      if (peer) {
        bool ready =
            video_->delivered_buffer.load(std::memory_order_acquire);
        if (!ready) {
          const auto ready_deadline =
              std::chrono::steady_clock::now() + kLiveEosReadyTimeout;
          while (!stop.stop_requested() &&
                 std::chrono::steady_clock::now() < ready_deadline) {
            GstCaps* caps = gst_pad_get_current_caps(peer);
            const bool negotiated = caps && !gst_caps_is_empty(caps);
            if (caps) gst_caps_unref(caps);
            ready = negotiated && video_->delivered_buffer.load(
                                      std::memory_order_acquire);
            if (ready) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
        }
        live_video_eos =
            ready && push_live_eos_when_idle(peer, video_->reserved_video_pad,
                                             stop, kLiveEosLockTimeout);
        gst_object_unref(peer);
      } else {
        live_video_eos = false;
      }
      if (!live_video_eos && !stop.stop_requested())
        g_warning("misbklv: failed to send EOS through the live video branch");
    }

    GstBus* bus = gst_element_get_bus(pipeline_);
    bool ok = false;
    bool cancelled = stop.stop_requested();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::nanoseconds(kFinishDrainTimeout);
    // Cap each poll short so a cancellation is noticed promptly. A realtime
    // file replay drains at wall-clock speed, so this is where a Ctrl-C after
    // the final KLV packet must take effect (ADR 0032).
    while (!cancelled && klv_eos == GST_FLOW_OK && live_video_eos &&
           std::chrono::steady_clock::now() < deadline) {
      if (stop.stop_requested()) {
        cancelled = true;
        break;
      }
      const auto remain = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
      const GstClockTime poll = remain > kFinishDrainPoll
                                    ? kFinishDrainPoll
                                    : static_cast<GstClockTime>(remain);
      GstMessage* m = gst_bus_timed_pop_filtered(
          bus, poll,
          static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
      if (!m) continue;
      if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS) {
        ok = true;
        gst_message_unref(m);
        break;
      }
      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(m, &err, &dbg);
      g_warning("misbklv: pipeline error during finish: %s",
                err ? err->message : "unknown");
      if (err) g_error_free(err);
      g_free(dbg);
      gst_message_unref(m);
      break;
    }
    if (!ok && !cancelled && std::chrono::steady_clock::now() >= deadline)
      g_warning("misbklv: pipeline did not drain within %" G_GUINT64_FORMAT
                "s of EOS; giving up",
                static_cast<guint64>(kFinishDrainTimeout / GST_SECOND));
    gst_object_unref(bus);

    const bool quiesced = quiesce_to_null();
    if (!quiesced) {
      ok = false;
      cancelled = false;
    }
    // Three outcomes: a clean EOS keeps the output; a caller cancellation and a
    // drain failure both discard any partial sink file (ADR 0022 — no output
    // unless the session succeeded). Cancellation is a caller request, not a
    // backend fault, so it returns ok — matching extract()'s cooperative-stop
    // convention (ADR 0019). A cancelled file sink therefore leaves no
    // half-written output, and a cancelled live sink simply stops.
    if (ok)
      removable_sink_.clear();
    else
      discard_output();
    if (ok || cancelled) return Result<std::monostate>::ok({});
    return Result<std::monostate>::err(Error::Backend);
  }

 private:
  // Sever every probe holding VideoCtx, then take the pipeline to NULL before
  // freeing it. remove_probes() blocks until any in-flight callback returns;
  // this closes the issue #57 SEI-probe use-after-free and also owns the live
  // delivery tracker's callback lifetime.
  //
  // Idempotent: finish() and the destructor both call it, and
  // remove_probes()/a repeated NULL are no-ops the second time.
  bool quiesce_to_null() {
    if (!pipeline_) return true;
    if (video_) video_->remove_probes();
    if (gst_element_set_state(pipeline_, GST_STATE_NULL) ==
        GST_STATE_CHANGE_FAILURE) {
      g_warning("misbklv: failed to request NULL pipeline state");
      return false;
    }
    GstState cur = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    const GstStateChangeReturn state = gst_element_get_state(
        pipeline_, &cur, &pending, kNullStateTimeout);
    if (state == GST_STATE_CHANGE_FAILURE || cur != GST_STATE_NULL) {
      g_warning("misbklv: pipeline did not reach NULL within %" G_GUINT64_FORMAT
                "s; continuing bounded teardown",
                static_cast<guint64>(kNullStateTimeout / GST_SECOND));
      return false;
    }
    return true;
  }

  void discard_output() {
    // Called after NULL was requested. On a bounded NULL-confirmation failure,
    // POSIX unlink still removes the directory entry while filesink unwinds.
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
  VideoSource vsrc = parse_video_source(cfg.video_source);
  if (vsrc.kind == VideoSourceKind::Unsupported)
    return R::err(Error::Unsupported);
  if (vsrc.kind == VideoSourceKind::File) {
    if (vsrc.spec.empty()) return R::err(Error::Unsupported);
    // Validate before building: filesink creates a file as soon as the pipeline
    // leaves NULL, and a failed open must not leave a partial output behind.
    std::FILE* f = std::fopen(vsrc.spec.c_str(), "rb");
    if (!f) return R::err(Error::Backend);
    std::fclose(f);
  } else if (vsrc.kind == VideoSourceKind::Pipeline) {
    if (vsrc.spec.empty()) return R::err(Error::Unsupported);
  } else if (vsrc.kind == VideoSourceKind::Rtsp) {
    if (vsrc.spec.empty()) return R::err(Error::Unsupported);
  }
  std::string sink_path;
  bool sink_preexisted = true;
  if (cfg.sink.rfind("file:", 0) == 0) {
    // Keep enough ownership state to remove only files this session created.
    // Probe *existence*, not readability: a pre-existing write-only file makes a
    // read-mode fopen fail, which would misclassify it as "created by us" and let
    // the failure path std::remove() a file that was the caller's — the exact
    // deletion ADR 0022 forbids. std::filesystem::exists asks the right question.
    sink_path = cfg.sink.substr(5);
    std::error_code ec;
    sink_preexisted = std::filesystem::exists(sink_path, ec);
    // A stat error (e.g. a permission-denied path component) makes exists()
    // return false with ec set. Treat that as pre-existing — ADR 0022's
    // guarantee is to *never* delete a caller's file, and on doubt we must
    // not delete. Only a clean "does not exist" makes the sink ours to remove.
    if (ec) sink_preexisted = true;
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
  GstElement* sink = make_sink(cfg.sink, cfg);
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
  const bool have_video = vsrc.kind != VideoSourceKind::None;
  if (have_video) {
    reserved_video_pad = gst_element_request_pad_simple(mux, "sink_%d");
    if (!reserved_video_pad) return fail(pipeline, Error::Backend);
    gst_object_unref(reserved_video_pad);
    // Link the KLV appsrc onto its own requested pad, allocated after the
    // video pad and so one PID above it. This must be an explicit request:
    // gst_element_link resolves the sink through gst_element_get_compatible_pad,
    // which hands back the existing unlinked reserved video pad rather than
    // allocating a new one — giving the metadata stream the video's PMT slot.
    // A wildcard request never reuses an existing pad, so no name arithmetic
    // is needed to stay above the video.
    GstPad* klv_pad = gst_element_request_pad_simple(mux, "sink_%d");
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
  if (have_video) {
    // Dynamic pads are linked by the video unit through parsers where required;
    // nothing is decoded, preserving codec passthrough.
    auto prepared =
        prepare_video_branch(pipeline, reserved_video_pad, mux, vsrc,
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
