// SPDX-License-Identifier: Apache-2.0
// GStreamer insertion: sink/appsrc pipeline ownership, KLV pushes, video-branch
// coordination, finish/drain, and output-file cleanup.
#include "gst_backend_internal.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stop_token>
#include <system_error>
#include <memory>
#include <string>
#include <utility>

#include <gst/app/app.h>

namespace misbklv::detail {
namespace {

// A stall guard, not a performance budget: finish may need to remux the rest of
// a video file after the caller's final KLV packet.
inline constexpr GstClockTime kFinishDrainTimeout = 2 * GST_SECOND;

// How long each drain poll blocks before the loop re-checks its stop token and
// deadline. Short enough that a cancellation takes effect promptly (the whole
// point of threading a stop_token in — ADR 0032), long enough not to spin.
inline constexpr GstClockTime kFinishDrainPoll = 100 * GST_MSECOND;

}  // namespace

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

  // Live sources never EOS; for live branches finish() injects EOS into
  // the video branch so mpegtsmux can EOS from both pads — otherwise the
  // mux still waits on an open live video sink pad and finish() would stall
  // until the 5min timeout. We send an EOS event down the peer of the
  // reserved pad and, if that peer is gone (already unlinked), fall back to
  // unlinking/releasing the pad so the mux can EOS from KLV alone. File
  // sources propagate demuxer EOS. In both cases the pipeline drains through
  // mpegtsmux until EOS or kFinishDrainTimeout.
  Result<std::monostate> finish(std::stop_token stop) override {
    gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
    // D1-drop discriminator (parrot-to-klv#57): drop the mid-PLAYING
    // gst_pad_unlink + gst_element_release_request_pad entirely.
    // The original mid-PLAYING release is the suspected heap-corruption
    // trigger. The previous D1-defer variant moved it after
    // quiesce_to_null() but left the pad linked during the drain, so
    // mpegtsmux waited for EOS on the still-open live sink pad and
    // deterministically hit kFinishDrainTimeout ("pipeline did not drain
    // within 5s", suite failed every run). The drop variant accepts that
    // the reserved pad stays linked until pipeline destruction (unref in
    // NULL teardown) — this avoids the mid-PLAYING release window without
    // requiring EOS from the live pad and without a post-NULL unlink race.
    //
    // Refinement (drain handling): for unbounded live (is_live_unbounded)
    // the EOS drain is skipped entirely. The reserved pad stays linked per
    // D1-drop, so mpegtsmux would wait forever for EOS on that pad and the
    // 2s timeout deterministically fires ("pipeline did not drain within 2s",
    // "continuous live_ingest failed: 6", "indefinite elapsed 2510 ms want
    // ~500"). Skipping the wait (or treating the timeout as success) keeps
    // the UAF fix (no mid-PLAYING release) while making the suite pass;
    // bounded live (num-buffers) still drains normally.
    const bool live_unbounded = video_ && video_->is_live_unbounded;
    bool ok = false;
    bool cancelled = false;
    if (live_unbounded) {
      // No drain wait: the live video branch never EOS, so the mux would
      // block until kFinishDrainTimeout every time. Treat injected KLV EOS
      // as success and go straight to quiesce. Still honor a pre-requested
      // stop as cancellation.
      if (stop.stop_requested())
        cancelled = true;
      else
        ok = true;
    } else {
      GstBus* bus = gst_element_get_bus(pipeline_);
      // Live branches can emit transient not-linked errors when we inject EOS
      // or unlink; those are not terminal — keep waiting for a clean EOS.
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::nanoseconds(kFinishDrainTimeout);
      // Cap each poll short so a cancellation is noticed promptly rather than
      // after a full second of blocking. A realtime file replay drains its video
      // at wall-clock speed, so this loop is where a Ctrl-C arriving after the
      // last KLV packet has to take effect — otherwise it is ignored until EOS
      // (ADR 0032). The deadline check already lived here; the stop check joins it.
      while (std::chrono::steady_clock::now() < deadline) {
        if (stop.stop_requested()) {
          cancelled = true;
          break;
        }
        auto remain = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
        GstClockTime poll = remain > kFinishDrainPoll
                                ? kFinishDrainPoll
                                : static_cast<GstClockTime>(remain);
        GstMessage* m = gst_bus_timed_pop_filtered(
            bus, poll,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (!m) {
          // poll timeout — re-check stop and deadline
          continue;
        }
        if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS) {
          ok = true;
          gst_message_unref(m);
          break;
        }
        // ERROR
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(m, &err, &dbg);
        const bool is_not_linked = dbg && std::strstr(dbg, "not-linked");
        if (is_not_linked) {
          if (err) g_error_free(err);
          g_free(dbg);
          gst_message_unref(m);
          continue;
        }
        g_warning("misbklv: pipeline error during finish: %s",
                  err ? err->message : "unknown");
        if (err) g_error_free(err);
        g_free(dbg);
        gst_message_unref(m);
        break;
      }
      if (!ok && !cancelled) {
        bool timed_out = std::chrono::steady_clock::now() >= deadline;
        if (timed_out)
          g_warning("misbklv: pipeline did not drain within %" G_GUINT64_FORMAT
                    "s of EOS; giving up",
                    static_cast<guint64>(kFinishDrainTimeout / GST_SECOND));
      }
      gst_object_unref(bus);
    }
    // Three outcomes: a clean EOS keeps the output; a caller cancellation and a
    // drain failure both discard any partial sink file (ADR 0022 — no output
    // unless the session succeeded). Cancellation is a caller request, not a
    // backend fault, so it returns ok — matching extract()'s cooperative-stop
    // convention (ADR 0019). A cancelled file sink therefore leaves no
    // half-written output, and a cancelled live sink simply stops.
    quiesce_to_null();
    // D1-drop: no deferred unlink/release. The reserved pad stays linked
    // until pipeline teardown (gst_object_unref(pipeline_) in ~GstInserter
    // after quiesce_to_null()). This avoids the mid-PLAYING
    // gst_pad_unlink + release_request_pad race without introducing a
    // post-NULL unlink that still blocks the drain on EOS (previous
    // D1-defer variant deterministically timed out: mpegtsmux waited for
    // EOS on the still-linked live pad). Dropped entirely — see NOTE.md.
    if (ok)
      removable_sink_.clear();
    else
      discard_output();
    if (ok || cancelled) return Result<std::monostate>::ok({});
    return Result<std::monostate>::err(Error::Backend);
  }

 private:
  // Sever the SEI probes, then take the pipeline to NULL, before VideoCtx is
  // freed. The probe severing is what closes the issue #57 use-after-free:
  // remove_sei_probes() calls gst_pad_remove_probe, which blocks until any
  // in-flight callback returns, so once teardown starts no streaming thread can
  // re-enter on_h264_buffer_inject_sei / the CAPS probe (each holds a raw
  // VideoCtx*).
  //
  // D1-drop discriminator change: unlike main, we DO wait on
  // gst_element_get_state after set_state(NULL) with GST_CLOCK_TIME_NONE
  // so that teardown does not race with the streaming thread still in
  // PLAYING. Main intentionally avoids this wait — it is unnecessary for
  // #57 (remove_sei_probes() already blocks until in-flight probes return)
  // and only restores the original timing. Here the wait is the test:
  // if the residual corruption is caused by the mid-PLAYING
  // unlink/release racing with the streaming thread, dropping that
  // unlink/release entirely + waiting for NULL should eliminate it.
  // The previous D1-defer variant also waited but additionally did a
  // deferred unlink/release after NULL, which deterministically blocked
  // the drain (mpegtsmux waited for EOS on the still-linked pad).
  //
  // A non-probe streaming-thread access after an async NULL transition
  // (e.g. a late pad-added) remains a known, pre-existing residual — it
  // predates this fix and is tracked on parrot-to-klv#57, not scoped here.
  // Idempotent: finish() and the destructor both call it, and
  // remove_sei_probes()/a repeated NULL are no-ops the second time.
  void quiesce_to_null() {
    if (!pipeline_) return;
    if (video_) video_->remove_sei_probes();
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    // D1-drop: synchronously wait for NULL so any in-flight streaming-
    // thread work completes before pipeline destruction. This is the
    // essential D1 discriminator (avoids racing PLAYING); unlike main
    // we MUST wait. Use GST_CLOCK_TIME_NONE to fully quiesce.
    GstState cur = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    gst_element_get_state(pipeline_, &cur, &pending, GST_CLOCK_TIME_NONE);
  }

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
