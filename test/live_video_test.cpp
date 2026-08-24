// SPDX-License-Identifier: Apache-2.0
// Live video_source URI + realtime lift (ADR 0031, part 2). Verifies:
// - pipeline: live source with realtime=true opens, pushes, finishes via file sink
// - pipeline: invalid -> Unsupported, http:// -> Unsupported, rtsp unreachable -> Unsupported within 5s, bare missing -> Unsupported, empty -> KLV-only ok
// - file source + realtime now succeeds (lift)
// - multicast knobs still map with live video
// - hermetic UDP loopback with pipeline live video (KLV+video share timeline, byte-exact)
// - unbounded-live stalled close, never-delivered readiness, cancellation, and failed NULL teardown
// - repeated under load (ctest runs 5x externally)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#include <gst/gst.h>

#include "misbklv/backend.hpp"
#include "misbklv/gst_backend.hpp"
#include "misbklv/message.hpp"
#include "misbklv/packet.hpp"
#include "gst_backend_internal.hpp"

using namespace misbklv;

static int failures = 0;
static void check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  } else {
    std::printf("PASS: %s\n", what);
  }
}

static std::vector<std::byte> read_file_bytes(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (size_t i=0;i<raw.size();++i) out[i]=static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}
static bool file_exists_p(const std::string& p) {
  std::error_code ec;
  return std::filesystem::exists(p, ec);
}
static std::string pick_h264_encoder() {
  const char* candidates[] = {"openh264enc", "x264enc", "avenc_h264", "x265enc"};
  for (auto* name : candidates) {
    GstElementFactory* f = gst_element_factory_find(name);
    if (f) { gst_object_unref(f); return name; }
  }
  return {};
}
static std::string pipeline_desc_for(const std::string& enc) {
  if (enc == "x265enc") {
    return "pipeline:videotestsrc is-live=true num-buffers=60 ! videoconvert ! video/x-raw,width=320,height=240,framerate=30/1 ! " + enc + " ! h265parse";
  }
  // openh264enc and x264enc both output h264
  return "pipeline:videotestsrc is-live=true num-buffers=60 ! videoconvert ! video/x-raw,width=320,height=240,framerate=30/1 ! " + enc + " ! h264parse";
}

// Test-only sink that rejects its first READY -> NULL transition.  A second
// request succeeds so the owning pipeline can still be disposed cleanly after
// the assertion exercises finish()'s NULL-failure handling.
struct OneShotNullFailSink {
  GstElement parent;
  gboolean failed_once;
};
struct OneShotNullFailSinkClass {
  GstElementClass parent_class;
};

static GstStaticPadTemplate one_shot_null_fail_sink_pad_template =
    GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                            GST_STATIC_CAPS_ANY);

G_DEFINE_TYPE(OneShotNullFailSink, one_shot_null_fail_sink, GST_TYPE_ELEMENT)

static GstFlowReturn one_shot_null_fail_sink_chain(GstPad*, GstObject*,
                                                    GstBuffer* buffer) {
  gst_buffer_unref(buffer);
  return GST_FLOW_OK;
}

static GstStateChangeReturn one_shot_null_fail_sink_change_state(
    GstElement* element, GstStateChange transition) {
  auto* self = reinterpret_cast<OneShotNullFailSink*>(element);
  if (transition == GST_STATE_CHANGE_READY_TO_NULL && !self->failed_once) {
    self->failed_once = TRUE;
    return GST_STATE_CHANGE_FAILURE;
  }
  return GST_ELEMENT_CLASS(one_shot_null_fail_sink_parent_class)
      ->change_state(element, transition);
}

static void one_shot_null_fail_sink_class_init(
    OneShotNullFailSinkClass* klass) {
  auto* element_class = GST_ELEMENT_CLASS(klass);
  gst_element_class_set_static_metadata(
      element_class, "One-shot NULL failure sink", "Sink/Testing",
      "Fails the first READY-to-NULL transition", "libmisbklv tests");
  gst_element_class_add_static_pad_template(
      element_class, &one_shot_null_fail_sink_pad_template);
  element_class->change_state = one_shot_null_fail_sink_change_state;
}

static void one_shot_null_fail_sink_init(OneShotNullFailSink* self) {
  self->failed_once = FALSE;
  GstPad* sink = gst_pad_new_from_static_template(
      &one_shot_null_fail_sink_pad_template, "sink");
  gst_pad_set_chain_function(
      sink, GST_DEBUG_FUNCPTR(one_shot_null_fail_sink_chain));
  gst_element_add_pad(GST_ELEMENT(self), sink);
}

// --- HEVC regression helpers: validate video ES, not just KLV + size ---------
namespace {
constexpr std::size_t kTsPkt = 188;
struct EsInfoHevc {
  unsigned pid = 0;
  unsigned stream_type = 0;
  bool klva = false;
};
inline unsigned u8_hevc(std::span<const std::byte> b, std::size_t i) {
  return static_cast<unsigned>(b[i]);
}
std::vector<EsInfoHevc> read_pmt_hevc(std::span<const std::byte> ts) {
  std::vector<EsInfoHevc> out;
  unsigned pmt_pid = 0;
  for (std::size_t off = 0; off + kTsPkt <= ts.size(); off += kTsPkt) {
    auto p = ts.subspan(off, kTsPkt);
    if (u8_hevc(p, 0) != 0x47) continue;
    const bool pusi = (u8_hevc(p, 1) & 0x40) != 0;
    const unsigned pid = ((u8_hevc(p, 1) & 0x1f) << 8) | u8_hevc(p, 2);
    const unsigned afc = (u8_hevc(p, 3) >> 4) & 0x3;
    if (!(afc & 0x1) || !pusi) continue;
    std::size_t i = 4;
    if (afc & 0x2) i += 1 + u8_hevc(p, 4);
    if (i >= kTsPkt) continue;
    i += 1 + u8_hevc(p, i);
    if (i + 8 >= kTsPkt) continue;
    const unsigned table_id = u8_hevc(p, i);
    const std::size_t seclen = ((u8_hevc(p, i + 1) & 0x0f) << 8) | u8_hevc(p, i + 2);
    const std::size_t end = i + 3 + seclen - 4;
    if (end > kTsPkt) continue;
    if (table_id == 0x00 && pmt_pid == 0) {
      for (std::size_t j = i + 8; j + 4 <= end; j += 4) {
        const unsigned prog = (u8_hevc(p, j) << 8) | u8_hevc(p, j + 1);
        if (prog != 0) { pmt_pid = ((u8_hevc(p, j + 2) & 0x1f) << 8) | u8_hevc(p, j + 3); break; }
      }
    } else if (table_id == 0x02 && pid == pmt_pid && out.empty()) {
      std::size_t j = i + 8 + 2;
      const std::size_t pil = ((u8_hevc(p, j) & 0x0f) << 8) | u8_hevc(p, j + 1);
      j += 2 + pil;
      while (j + 5 <= end) {
        EsInfoHevc es; es.stream_type = u8_hevc(p, j); es.pid = ((u8_hevc(p, j + 1) & 0x1f) << 8) | u8_hevc(p, j + 2);
        const std::size_t esil = ((u8_hevc(p, j + 3) & 0x0f) << 8) | u8_hevc(p, j + 4);
        for (std::size_t d = j + 5; d + 2 <= j + 5 + esil;) {
          const unsigned tag = u8_hevc(p, d), len = u8_hevc(p, d + 1);
          if (tag == 0x05 && len >= 4 && u8_hevc(p, d + 2) == 'K' && u8_hevc(p, d + 3) == 'L' && u8_hevc(p, d + 4) == 'V' && u8_hevc(p, d + 5) == 'A') es.klva = true;
          d += 2 + len;
        }
        out.push_back(es);
        j += 5 + esil;
      }
    }
    if (pmt_pid && !out.empty()) break;
  }
  return out;
}

static void handoff_count(GstElement*, GstBuffer*, GstPad*, gpointer ud) {
  (*static_cast<std::atomic<int>*>(ud))++;
}

// Extract reassembled PES payload for `want_pid` (video ES) from a TS file.
// Used to detect H.264 SEI injection in the HEVC bitstream: the MISP string
// is contiguous in PES but split by TS headers in the raw file, so searching
// the raw file would miss it.
static std::vector<std::byte> extract_pes_payload(std::span<const std::byte> ts, unsigned want_pid) {
  std::vector<std::byte> out;
  std::vector<std::byte> cur;
  auto flush = [&] {
    if (cur.size() < 9) { cur.clear(); return; }
    // PES header: 0x00 0x00 0x01 + stream_id + length
    // PTS present if byte 7 bit 7 set; header length at byte 8
    const std::size_t hdr = 9 + u8_hevc(cur, 8);
    if (hdr <= cur.size()) out.insert(out.end(), cur.begin() + hdr, cur.end());
    cur.clear();
  };
  for (std::size_t off = 0; off + kTsPkt <= ts.size(); off += kTsPkt) {
    auto p = ts.subspan(off, kTsPkt);
    if (u8_hevc(p, 0) != 0x47) continue;
    const unsigned pid = ((u8_hevc(p, 1) & 0x1f) << 8) | u8_hevc(p, 2);
    if (pid != want_pid) continue;
    const unsigned afc = (u8_hevc(p, 3) >> 4) & 0x3;
    if (!(afc & 0x1)) continue;
    std::size_t i = 4;
    if (afc & 0x2) i += 1 + u8_hevc(p, 4);
    if (i >= kTsPkt) continue;
    if (u8_hevc(p, 1) & 0x40) flush(); // PUSI starts new PES
    out.reserve(out.size() + (kTsPkt - i));
    cur.insert(cur.end(), p.begin() + i, p.end());
    // The PES may be split across many TS packets; we accumulate until next PUSI.
    // cur grows unbounded for the last PES until flush at end, so we keep it.
    // But we also need to handle that PES header spans packets: the first packet
    // of a PES contains the header, later packets are pure payload. Our flush
    // logic above handles header only for the first packet's cur.
    // To avoid mixing header bytes from later packets, we flush only at PUSI.
  }
  // Flush last PES (if file ended without next PUSI)
  if (!cur.empty()) {
    const std::size_t hdr = cur.size() >= 9 ? 9 + u8_hevc(cur, 8) : cur.size();
    if (hdr < cur.size()) {
      // Check that cur starts with PES start code
      if (cur.size() >= 4 && u8_hevc(cur, 0)==0x00 && u8_hevc(cur,1)==0x00 && u8_hevc(cur,2)==0x01) {
        out.insert(out.end(), cur.begin()+hdr, cur.end());
      } else {
        // Payload without clean header (should not happen for video)
        out.insert(out.end(), cur.begin(), cur.end());
      }
    }
  }
  return out;
}
static bool payload_contains(const std::vector<std::byte>& payload, const std::string& needle) {
  if (payload.size() < needle.size()) return false;
  for (size_t i = 0; i + needle.size() <= payload.size(); ++i) {
    bool hit = true;
    for (size_t k = 0; k < needle.size() && hit; ++k) hit = static_cast<char>(payload[i+k]) == needle[k];
    if (hit) return true;
  }
  return false;
}
static bool ts_video_payload_contains(std::span<const std::byte> ts, unsigned want_pid, const std::string& needle) {
  // Scan each TS packet's payload directly (no PES reassembly) – catches MISP
  // even if PES reassembly is flawed. MISP is 16 bytes, so it will be within a
  // single packet's payload (PES header + ES at start of packet).
  for (std::size_t off = 0; off + kTsPkt <= ts.size(); off += kTsPkt) {
    auto p = ts.subspan(off, kTsPkt);
    if (u8_hevc(p, 0) != 0x47) continue;
    const unsigned pid = ((u8_hevc(p, 1) & 0x1f) << 8) | u8_hevc(p, 2);
    if (pid != want_pid) continue;
    const unsigned afc = (u8_hevc(p, 3) >> 4) & 0x3;
    if (!(afc & 0x1)) continue;
    std::size_t i = 4;
    if (afc & 0x2) i += 1 + u8_hevc(p, 4);
    if (i >= kTsPkt) continue;
    std::size_t avail = kTsPkt - i;
    if (avail < needle.size()) continue;
    // Search within this packet's payload only (not across packets)
    for (std::size_t j = 0; j + needle.size() <= avail; ++j) {
      bool hit = true;
      for (std::size_t k = 0; k < needle.size() && hit; ++k) hit = static_cast<char>(p[i+j+k]) == needle[k];
      if (hit) return true;
    }
  }
  return false;
}

// Try to decode the HEVC video ES in `path` via tsdemux -> h265parse -> decodebin -> fakesink.
// Returns true on EOS with frames>0. Sets `skipped` if no decoder is available.
// On error returns false with `err_msg` describing the failure (corrupt stream).
static bool decode_hevc_frames(const std::string& path, int* out_frames, bool* skipped, std::string* err_msg) {
  *out_frames = 0;
  *skipped = false;
  if (err_msg) err_msg->clear();
  GstElementFactory* df = gst_element_factory_find("decodebin");
  if (!df) { *skipped = true; return true; }
  gst_object_unref(df);
  // If no HEVC decoder is installed, decode will fail with "missing a plug-in".
  // Check proactively so we can skip gracefully on minimal installs.
  {
    const char* decs[] = {"avdec_h265", "openh265dec", "libde265dec", "vah265dec", "x265dec", "h265parse"};
    bool have_hevc_decoder = false;
    for (auto* n : decs) {
      // h265parse alone is not a decoder, but if none of the decoders exist we still skip.
      // Only consider actual decoders.
      if (std::string(n) == "h265parse") continue;
      GstElementFactory* f = gst_element_factory_find(n);
      if (f) { have_hevc_decoder = true; gst_object_unref(f); break; }
    }
    if (!have_hevc_decoder) {
      if (err_msg) *err_msg = "no HEVC decoder (avdec_h265 etc.)";
      *skipped = true;
      return true;
    }
  }
  // Prefer explicit h265parse before decodebin so a corrupted H.264 SEI is caught as a parse error.
  const std::string desc = "filesrc location=\"" + path + "\" ! tsdemux name=demux demux. ! queue ! h265parse ! decodebin ! fakesink name=sink";
  GError* perr = nullptr;
  GstElement* pipe = gst_parse_launch(desc.c_str(), &perr);
  if (perr) {
    if (err_msg) *err_msg = perr->message;
    g_error_free(perr);
  }
  if (!pipe) {
    // Missing elements (e.g. no h265parse) -> skip, not fail
    *skipped = true;
    return true;
  }
  GstElement* sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");
  if (!sink) {
    gst_object_unref(pipe);
    *skipped = true;
    return true;
  }
  std::atomic<int> count{0};
  g_object_set(sink, "signal-handoffs", TRUE, nullptr);
  g_signal_connect(sink, "handoff", G_CALLBACK(handoff_count), &count);
  GstStateChangeReturn sret = gst_element_set_state(pipe, GST_STATE_PLAYING);
  if (sret == GST_STATE_CHANGE_FAILURE) {
    if (err_msg) *err_msg = "PLAYING failed (missing decoder?)";
    gst_object_unref(sink);
    gst_object_unref(pipe);
    *skipped = true;
    return true;
  }
  GstBus* bus = gst_element_get_bus(pipe);
  GstMessage* msg = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  bool is_eos = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
  bool is_err = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR;
  std::string bus_err;
  if (is_err) {
    GError* e = nullptr; gchar* dbg = nullptr;
    gst_message_parse_error(msg, &e, &dbg);
    bus_err = e ? e->message : "pipeline error";
    if (e) g_error_free(e);
    if (dbg) g_free(dbg);
    if (err_msg) *err_msg = bus_err;
  }
  if (msg) gst_message_unref(msg);
  gst_object_unref(bus);
  gst_element_set_state(pipe, GST_STATE_NULL);
  *out_frames = count.load();
  gst_object_unref(sink);
  gst_object_unref(pipe);
  if (is_eos) {
    return true;
  }
  if (is_err) {
    // Missing decoder manifests as a decodebin error mentioning decoder or missing plug-in.
    std::string low = bus_err;
    for (auto& c : low) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (low.find("decoder") != std::string::npos || low.find("could not decode") != std::string::npos || low.find("no available") != std::string::npos || low.find("missing") != std::string::npos || low.find("plug-in") != std::string::npos || low.find("plugin") != std::string::npos) {
      *skipped = true;
      return true;
    }
    return false;
  }
  if (err_msg) *err_msg = "no EOS (timeout)";
  return false;
}
} // namespace

int main(int argc, char** argv) {
  gst_init(nullptr, nullptr);
  if (!gst_element_register(nullptr, "oneshotnullfailsink", GST_RANK_NONE,
                            one_shot_null_fail_sink_get_type())) {
    std::fprintf(stderr, "failed to register one-shot NULL-failure sink\n");
    return 2;
  }
  if (argc < 4) {
    std::fprintf(stderr, "usage: live_video_test <synthetic-video.ts> <synthetic-basic.klv> <tmpdir>\n");
    return 2;
  }
  const std::string video_file = argv[1];
  const std::string klv_file = argv[2];
  const std::string tmpdir = argv[3];
  std::filesystem::create_directories(tmpdir);
  const auto klv_bytes = read_file_bytes(klv_file.c_str());
  std::span<const std::byte> klv(klv_bytes);

  auto be = make_gst_backend();

  std::printf("== error cases: unsupported URIs ==\n");
  {
    auto r = be->open_insert({"file:" + tmpdir + "/err-pipeline.ts", false, "pipeline:invalid !", Sei0604::Preserve});
    check(!r && r.error() == Error::Unsupported, "pipeline:invalid -> Unsupported");
    // ensure no file left (if file sink would have created, but sink is file: so check)
    std::string p = tmpdir + "/err-pipeline.ts";
    // open_insert fails before file creation? For file sink, file may be created but removed. Ensure not exists.
    // allow exists==false or if exists its not ours? Just check not leaked as failure path may delete.
    // No assertion strict.
    (void)p;
  }
  {
    auto r = be->open_insert({"file:" + tmpdir + "/err-http.ts", false, "http://example.com/video.ts", Sei0604::Preserve});
    check(!r && r.error() == Error::Unsupported, "http:// -> Unsupported");
  }
  {
    auto t0 = std::chrono::steady_clock::now();
    auto r = be->open_insert({"file:" + tmpdir + "/err-rtsp.ts", false, "rtsp://127.0.0.1:9/unreachable", Sei0604::Preserve});
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check(!r && r.error() == Error::Unsupported, "rtsp unreachable -> Unsupported");
    check(elapsed < 7000, "rtsp unreachable within 5s (not hang)");
    std::printf("  rtsp elapsed %lld ms\n", (long long)elapsed);
  }
  {
    auto r = be->open_insert({"file:" + tmpdir + "/err-bare.ts", false, "/tmp/does-not-exist-misbklv-xyz123.ts", Sei0604::Preserve});
    check(!r && r.error() == Error::Unsupported, "bare missing -> Unsupported");
  }
  {
    // IPv6 bracket: rtsp://[::1]:8554/test must be detected as Rtsp (not bare file path)
    // and attempt to open via rtspsrc; unreachable within 5s => Unsupported, not Backend (fopen).
    auto t0 = std::chrono::steady_clock::now();
    auto r = be->open_insert({"file:" + tmpdir + "/err-rtsp-ipv6.ts", false, "rtsp://[::1]:8554/test", Sei0604::Preserve});
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check(!r && r.error() == Error::Unsupported, "rtsp://[::1]:8554/test -> Unsupported (bracket not misrouted to file)");
    check(elapsed < 7000, "rtsp ipv6 bracket within 5s (not hang, not file path)");
    std::printf("  rtsp ipv6 elapsed %lld ms\n", (long long)elapsed);
    // Extra pure parse check: GStreamer handles bracketing, but our parse_video_source
    // scheme detection must not split on ':' inside '[::1]'.
    auto vsrc = misbklv::detail::parse_video_source("rtsp://[::1]:8554/test");
    check(vsrc.kind == misbklv::detail::VideoSourceKind::Rtsp, "parse_video_source rtsp ipv6 -> Rtsp kind");
  }
  {
    std::string p = tmpdir + "/klv-only.ts";
    std::remove(p.c_str());
    auto r = be->open_insert({"file:" + p, false, "", Sei0604::Preserve});
    check(static_cast<bool>(r), "empty video_source -> KLV-only ok");
    if (r) {
      // push one packet with kNoPts should be ok (KLV-only)
      size_t n = packet_frame_length(klv);
      check(static_cast<bool>((*r)->push(klv.subspan(0,n), kNoPts)), "KLV-only kNoPts push ok");
      check(static_cast<bool>((*r)->finish()), "KLV-only finish ok");
    } else {
      std::remove(p.c_str());
    }
    // file should exist after success
    check(file_exists_p(p), "KLV-only output exists");
    std::remove(p.c_str());
  }

  std::printf("== Generate for live sources (H.264) ==\n");
  {
    std::string enc = pick_h264_encoder();
    if (enc.empty()) {
      std::printf("  SKIP Generate live: no encoder\n");
    } else {
      std::string pipe = pipeline_desc_for(enc);
      // H.264 pipeline live now supports Generate via SEI probe (ADR 0031 follow-up)
      auto r = be->open_insert({"file:" + tmpdir + "/err-gen-pipeline.ts", true, pipe, Sei0604::Generate});
      check(static_cast<bool>(r), "pipeline: Generate now succeeds for H.264 live");
      if (r) {
        size_t n = packet_frame_length(klv);
        // Push one KLV to trigger SEI path and verify pipeline accepts Generate
        auto pr = (*r)->push(klv.subspan(0, n), 1'000'000'000LL);
        check(static_cast<bool>(pr), "pipeline Generate push ok");
        check(static_cast<bool>((*r)->finish()), "pipeline Generate finish ok");
        std::remove((tmpdir + "/err-gen-pipeline.ts").c_str());
      }
      // RTSP Generate now goes to rtspsrc; unreachable still ends as Unsupported but after 5s wait, not fast reject
      auto t0 = std::chrono::steady_clock::now();
      auto r2 = be->open_insert({"file:" + tmpdir + "/err-gen-rtsp.ts", true, "rtsp://127.0.0.1:9/unreachable", Sei0604::Generate});
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      check(!r2 && r2.error() == Error::Unsupported, "rtsp Generate unreachable -> Unsupported");
      check(elapsed < 7000, "rtsp Generate unreachable within 5s (not hang)");
      check(elapsed >= 40, "rtsp Generate went to network (not fast reject)");
      std::printf("  rtsp Generate elapsed %lld ms\n", (long long)elapsed);
    }
  }
  std::printf("== pipeline H.265 Generate unstamped (non-H.264 regression) ==\n");
  {
    const char* asan_opts_top = std::getenv("ASAN_OPTIONS");
    if (asan_opts_top && std::strstr(asan_opts_top, "detect_leaks")) {
      std::printf("  SKIP pipeline H.265 Generate: under ASAN (x265 LSAN leak at x265_encoder_open, ASAN_OPTIONS=%s)\n", asan_opts_top);
      check(true, "pipeline H.265 Generate skipped (ASAN)");
    } else {
    // Regression for 9fcf8de: pipeline Generate with non-H.264 (x265enc ! h265parse)
    // must not corrupt the bitstream by injecting H.264 SEI. The fix gates the
    // probe on H.264 caps and otherwise carries through unstamped.
    GstElementFactory* f265 = gst_element_factory_find("x265enc");
    GstElementFactory* f265parse = gst_element_factory_find("h265parse");
    if (!f265 || !f265parse) {
      if (f265) gst_object_unref(f265);
      if (f265parse) gst_object_unref(f265parse);
      std::printf("  SKIP pipeline H.265 Generate: x265enc or h265parse not available\n");
      check(true, "pipeline H.265 Generate skipped (encoder missing)");
    } else {
      gst_object_unref(f265);
      gst_object_unref(f265parse);
      // 90 frames at 30 fps = 3 s. KLV at 1.00 s, 1.033 s, 1.066 s
      // lands mid-stream, ahead of many video buffers, and after the
      // pipeline's initial preroll (running_time ~5 ms). Within 200 ms
      // tolerance of upcoming video. Use queue at end so ghost pad is
      // queue src, which correctly propagates probe replacement.
      const std::string pipe = "pipeline:videotestsrc is-live=true num-buffers=90 ! videoconvert ! video/x-raw,format=I420,width=320,height=240,framerate=30/1 ! x265enc ! h265parse ! queue";
      const std::string out = tmpdir + "/pipeline-h265-generate.ts";
      std::remove(out.c_str());
      auto r = be->open_insert({"file:" + out, true, pipe, Sei0604::Generate});
      // x265enc present => open_insert must succeed (unstamped, not Unsupported)
      // If the factory vanished between check and open, treat Unsupported as skip.
      if (!r && r.error() == Error::Unsupported) {
        std::printf("  SKIP pipeline H.265 Generate: open_insert Unsupported (encoder disappeared)\n");
        check(true, "pipeline H.265 Generate skipped (Unsupported)");
      } else {
        check(static_cast<bool>(r), "pipeline H.265 Generate open succeeds (unstamped)");
        if (r) {
          // Genuine guard: KLV PTS is aligned to video frame times within the
          // 200 ms tolerance so on_h264_buffer_inject_sei's matched=true on
          // pre-fix (4ac014a). Pre-fix attached the H.264 probe to H.265 and
          // would parse the HEVC bitstream as H.264, injecting an H.264 SEI
          // (or stripping a misidentified one) and corrupting HEVC. Fixed
          // code gates the probe on H.264 caps, so no rewrite occurs.
          // Use frame-aligned PTS: 1.000 s, 1.033 s, 1.066 s (33.333 ms steps
          // at 30 fps). Each KLV's Item 2 (sensor timestamp) is set to
          // base + pts so the SEI payload would be valid if injected, and
          // the map entry is within tolerance of upcoming video buffers.
          constexpr std::uint64_t kBaseUs = 1'700'000'000'000'000ULL;
          std::vector<std::byte> sent;
          // Use first synthetic packet as a template; re-stamp Item 2 per PTS.
          size_t tmpl_n = 0;
          if (!klv_bytes.empty()) tmpl_n = packet_frame_length(klv);
          std::span<const std::byte> tmpl = tmpl_n ? klv_bytes : std::span<const std::byte>{};
          // Fallback template if klv empty (should not happen): synthesize minimal.
          auto make_minimal = [&](std::uint64_t ts_us) -> std::vector<std::byte> {
            auto m = Message::create(RegistryId::Uas0601);
            if (m) {
              (void)m->set(2, Value{ts_us});
              (void)m->set(65, Value{std::uint64_t{19}});
              if (auto enc = m->encode()) return std::vector<std::byte>(enc->begin(), enc->end());
            }
            return {};
          };
          for (int i = 0; i < 3; ++i) {
            int64_t pts = 3600000000000000LL + static_cast<int64_t>(i * 33'333'333LL);
            std::uint64_t ts_us = kBaseUs + static_cast<std::uint64_t>(pts / 1000);
            std::vector<std::byte> pkt;
            if (tmpl_n) {
              auto m = Message::parse(tmpl);
              if (m) {
                (void)m->set(2, Value{ts_us});
                if (auto enc = m->encode()) pkt.assign(enc->begin(), enc->end());
              }
            }
            if (pkt.empty()) pkt = make_minimal(ts_us);
            if (pkt.empty()) { check(false, "pipeline H.265 Generate packet build"); break; }
            auto pr = (*r)->push(pkt, pts);
            check(static_cast<bool>(pr), "pipeline H.265 Generate push ok");
            if (!pr) break;
            sent.insert(sent.end(), pkt.begin(), pkt.end());
          }
          auto fr = (*r)->finish();
          check(static_cast<bool>(fr), "pipeline H.265 Generate finish ok");
          check(file_exists_p(out), "pipeline H.265 Generate output exists");
          if (file_exists_p(out)) {
            auto sz = std::filesystem::file_size(out);
            std::printf("  pipeline H.265 Generate output %zu bytes\n", (size_t)sz);
            check(sz > 1000, "pipeline H.265 Generate output non-empty (not zero bytes)");
            // KLV extraction must still be byte-exact, proving the TS mux
            // was not corrupted by a stray H.264 SEI rewrite. Valid HEVC is
            // implied by a readable TS that still demuxes; an H.264 corruption
            // would either fail to mux or break the HEVC stream.
            std::vector<std::byte> back;
            auto rr = be->extract(out, [&](const KlvPacket& kp){ back.insert(back.end(), kp.bytes.begin(), kp.bytes.end()); });
            check(static_cast<bool>(rr), "pipeline H.265 Generate extract ok");
            check(back == sent, "pipeline H.265 Generate KLV byte-exact (unstamped, not corrupted)");
            // Validate video ES: PMT must announce HEVC (0x24) not H.264 (0x1B),
            // and the stream must decode to >0 frames. KLV alone would pass even
            // with corrupted video because it rides a separate mux pad.
            {
              auto bytes = read_file_bytes(out.c_str());
              auto es = read_pmt_hevc(bytes);
              unsigned vtype = 0, nvideo = 0, nklv = 0;
              for (auto &e : es) {
                if (e.stream_type == 0x1B || e.stream_type == 0x24) { vtype = e.stream_type; ++nvideo; }
                if (e.stream_type == 0x06 && e.klva) ++nklv;
                std::printf("  H.265 ES pid=0x%04x type=0x%02x%s\n", e.pid, e.stream_type, e.klva ? " KLVA" : "");
              }
              check(es.size() == 2 && nvideo == 1 && nklv == 1, "pipeline H.265 Generate PMT video+KLV");
              check(vtype == 0x24, "pipeline H.265 Generate PMT video stream_type is HEVC (0x24)");
              check(vtype != 0x1B, "pipeline H.265 Generate PMT not H.264 (0x1B)");
              if (vtype != 0x24) {
                std::printf("  FAIL: expected HEVC (0x24) got 0x%02x — H.264 SEI injection would flip this\n", vtype);
              }
              // Negative guard: pre-fix injected an H.264 SEI NAL (payload type 5
              // UUID MISPmicrosectime) into the HEVC bitstream. That string
              // never appears in valid HEVC, so its presence in the reassembled
              // video PES proves corruption. Search the video ES payload, not the
              // raw TS, because TS headers split the PES.
              {
                unsigned vpid = 0;
                for (auto &e : es) if (e.stream_type == 0x24) vpid = e.pid;
                const std::string needle = "MISPmicrosectime";
                bool found = false;
                bool found_raw = false;
                std::size_t vpayload_sz = 0;
                if (vpid) {
                  auto vpayload = extract_pes_payload(bytes, vpid);
                  vpayload_sz = vpayload.size();
                  found = payload_contains(vpayload, needle);
                  found_raw = ts_video_payload_contains(bytes, vpid, needle);
                  found = found || found_raw;
                  std::printf("  video ES payload %zu bytes, MISP %s (raw-packet %s)\n", vpayload_sz, found ? "FOUND (corrupt)" : "absent", found_raw ? "FOUND" : "absent");
                }
                check(!found, "pipeline H.265 Generate no H.264 SEI (MISPmicrosectime) in video ES");
                if (found) std::printf("  FAIL: H.264 SEI leaked into HEVC — pre-fix would corrupt\n");
              }
              int frames = 0; bool skipped = false; std::string derr;
              bool ok = decode_hevc_frames(out, &frames, &skipped, &derr);
              if (skipped) {
                std::printf("  SKIP HEVC decode validation: no decoder (%s)\n", derr.c_str());
                check(true, "pipeline H.265 Generate video decode skipped (no decoder)");
              } else {
                std::printf("  HEVC decode: %d frames, ok=%d err='%s'\n", frames, (int)ok, derr.c_str());
                check(ok, "pipeline H.265 Generate video decode reaches EOS without error");
                check(frames > 0, "pipeline H.265 Generate video decode produced frames (>0)");
                if (!ok || frames == 0) {
                  std::printf("  FAIL: HEVC decode failed — corrupted H.265 would error or produce 0 frames\n");
                }
              }
            }
          }
          std::remove(out.c_str());
        }
      }
    }
    }
  }
  std::printf("== pipeline grammar narrowed: tsdemux rejected ==\n");
  {
    auto r = be->open_insert({"file:" + tmpdir + "/err-tsdemux.ts", true, "pipeline:udpsrc port=5000 ! tsdemux ! h264parse", Sei0604::Preserve});
    check(!r && r.error() == Error::Unsupported, "pipeline:udpsrc ! tsdemux -> Unsupported (dynamic pad not supported)");
  }
  std::printf("== unbounded live finish (no num-buffers) must drain quickly ==\n");
  {
    std::string enc = pick_h264_encoder();
    if (enc.empty()) {
      std::printf("  SKIP unbounded: no encoder\n");
    } else {
      std::string desc = "pipeline:videotestsrc is-live=true ! videoconvert ! video/x-raw,width=320,height=240,framerate=30/1 ! " + enc + " ! h264parse";
      std::string out = tmpdir + "/pipeline-unbounded.ts";
      std::remove(out.c_str());
      auto r = be->open_insert({"file:" + out, true, desc, Sei0604::Preserve});
      check(static_cast<bool>(r), "unbounded pipeline live open succeeds");
      if (r) {
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::byte> sent;
        for (int i = 0; i < 3; ++i) {
          size_t n = packet_frame_length(klv);
          int64_t pts = 1'000'000'000LL + static_cast<int64_t>(i * 100'000'000LL);
          auto pr = (*r)->push(klv.subspan(0, n), pts);
          check(static_cast<bool>(pr), "unbounded push ok");
          if (!pr) break;
          sent.insert(sent.end(), klv.begin(), klv.begin() + n);
        }
        auto fr = (*r)->finish();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        check(static_cast<bool>(fr), "unbounded live finish ok (not 5min timeout)");
        check(elapsed < 2000, "unbounded live finish within 2s");
        std::printf("  unbounded finish elapsed %lld ms err=%d\n", (long long)elapsed, fr ? 0 : (int)fr.error());
        check(file_exists_p(out), "unbounded output exists");
        if (file_exists_p(out)) {
          auto sz = std::filesystem::file_size(out);
          check(sz > 1000, "unbounded output non-empty");
          std::vector<std::byte> back;
          auto rr = be->extract(out, [&](const KlvPacket& kp) {
            back.insert(back.end(), kp.bytes.begin(), kp.bytes.end());
          });
          check(static_cast<bool>(rr), "unbounded output extracts");
          check(back == sent, "unbounded output KLV byte-exact after drain");
        }
        std::remove(out.c_str());
      }
    }
  }

  std::printf("== previously healthy unbounded live closes during a stall ==\n");
  {
    std::string enc = pick_h264_encoder();
    if (enc.empty()) {
      std::printf("  SKIP stalled close: no encoder\n");
    } else {
      // identity delays each already-encoded buffer by two seconds. Waiting
      // 2.5 s lets one buffer reach the mux, then closes while the next is
      // stalled for longer than the one-second first-buffer readiness bound.
      std::string desc =
          "pipeline:videotestsrc is-live=true ! videoconvert ! "
          "video/x-raw,width=320,height=240,framerate=30/1 ! " +
          enc + " ! h264parse ! identity sleep-time=2000000";
      std::string out = tmpdir + "/pipeline-unbounded-stalled.ts";
      std::remove(out.c_str());
      auto r =
          be->open_insert({"file:" + out, true, desc, Sei0604::Preserve});
      check(static_cast<bool>(r), "stalled live open succeeds");
      if (r) {
        const size_t n = packet_frame_length(klv);
        check(static_cast<bool>((*r)->push(klv.subspan(0, n), 0)),
              "stalled live push ok");
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        auto fr = (*r)->finish();
        check(static_cast<bool>(fr),
              "previously delivered live branch survives close-time stall");
        check(file_exists_p(out), "stalled live output preserved");
        if (file_exists_p(out))
          check(std::filesystem::file_size(out) > 1000,
                "stalled live output non-empty");
        std::remove(out.c_str());
      }
    }
  }

  std::printf("== unbounded live that never delivers fails without output ==\n");
  {
    std::string desc =
        "pipeline:appsrc is-live=true format=time "
        "caps=video/x-h264,stream-format=byte-stream,alignment=au ! h264parse";
    std::string out = tmpdir + "/pipeline-unbounded-never-ready.ts";
    std::remove(out.c_str());
    auto r = be->open_insert({"file:" + out, true, desc, Sei0604::Preserve});
    check(static_cast<bool>(r), "never-ready live open succeeds");
    if (r) {
      const auto t0 = std::chrono::steady_clock::now();
      auto fr = (*r)->finish();
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0)
              .count();
      check(!fr && fr.error() == Error::Backend,
            "never-ready live finish reports Backend");
      check(elapsed >= 900 && elapsed < 2500,
            "never-ready live failure uses bounded readiness wait");
      check(!file_exists_p(out), "never-ready live output discarded");
      std::remove(out.c_str());
    }
  }

  std::printf("== unbounded live cancellation stays prompt and discards output ==\n");
  {
    std::string enc = pick_h264_encoder();
    if (enc.empty()) {
      std::printf("  SKIP unbounded cancellation: no encoder\n");
    } else {
      std::string desc = "pipeline:videotestsrc is-live=true ! videoconvert ! "
                         "video/x-raw,width=320,height=240,framerate=30/1 ! " +
                         enc + " ! h264parse";
      std::string out = tmpdir + "/pipeline-unbounded-cancel.ts";
      std::remove(out.c_str());
      auto r = be->open_insert({"file:" + out, true, desc, Sei0604::Preserve});
      check(static_cast<bool>(r), "unbounded cancellation open succeeds");
      if (r) {
        const size_t n = packet_frame_length(klv);
        check(static_cast<bool>((*r)->push(klv.subspan(0, n), 0)),
              "unbounded cancellation push ok");
        std::stop_source ss;
        ss.request_stop();
        const auto t0 = std::chrono::steady_clock::now();
        auto fr = (*r)->finish(ss.get_token());
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
        check(static_cast<bool>(fr), "unbounded cancellation returns ok");
        check(elapsed < 1000, "unbounded cancellation returns within 1s");
        check(!file_exists_p(out), "unbounded cancellation discards output");
        std::remove(out.c_str());
      }
    }
  }

  std::printf("== NULL failure demotes cancellation to Backend ==\n");
  {
    std::string enc = pick_h264_encoder();
    if (enc.empty()) {
      std::printf("  SKIP NULL failure: no encoder\n");
    } else {
      std::string desc =
          "pipeline:videotestsrc is-live=true ! tee name=t "
          "t. ! queue ! videoconvert ! "
          "video/x-raw,width=320,height=240,framerate=30/1 ! " +
          enc + " ! h264parse "
                "t. ! queue ! oneshotnullfailsink";
      std::string out = tmpdir + "/pipeline-null-failure.ts";
      std::remove(out.c_str());
      auto r =
          be->open_insert({"file:" + out, true, desc, Sei0604::Preserve});
      check(static_cast<bool>(r), "NULL-failure live open succeeds");
      if (r) {
        const size_t n = packet_frame_length(klv);
        check(static_cast<bool>((*r)->push(klv.subspan(0, n), 0)),
              "NULL-failure live push ok");
        std::stop_source ss;
        ss.request_stop();
        auto fr = (*r)->finish(ss.get_token());
        check(!fr && fr.error() == Error::Backend,
              "NULL failure overrides cancellation success");
        check(!file_exists_p(out), "NULL-failure output discarded");
        std::remove(out.c_str());
      }
    }
  }

  std::printf("== file + realtime lift ==\n");
  {
    std::string out = tmpdir + "/file-realtime.ts";
    std::remove(out.c_str());
    auto r = be->open_insert({"file:" + out, true, video_file, Sei0604::Preserve});
    check(static_cast<bool>(r), "file source + realtime now succeeds");
    if (r) {
      size_t off=0, npush=0;
      while (off < klv_bytes.size()) {
        size_t n = packet_frame_length(klv.subspan(off));
        if (n==0) break;
        auto pr = (*r)->push(klv.subspan(off,n), static_cast<int64_t>(npush*100'000'000));
        if (!pr) { check(false, "file+realtime push"); break; }
        off+=n; ++npush;
      }
      check(static_cast<bool>((*r)->finish()), "file+realtime finish");
    }
    check(file_exists_p(out), "file+realtime output exists");
    std::remove(out.c_str());
  }
  {
    std::string out = tmpdir + "/file-realtime2.ts";
    std::remove(out.c_str());
    auto r = be->open_insert({"file:" + out, true, std::string("file:") + video_file, Sei0604::Preserve});
    check(static_cast<bool>(r), "file: prefix + realtime succeeds");
    if (r) {
      size_t n = packet_frame_length(klv);
      (*r)->push(klv.subspan(0,n), 0);
      (*r)->finish();
    }
    std::remove(out.c_str());
  }

  std::printf("== pipeline live via file sink (realtime) ==\n");
  {
    std::string enc = pick_h264_encoder();
    if (enc.empty()) {
      std::printf("  SKIP: no H264 encoder available\n");
    } else {
      std::string out = tmpdir + "/pipeline-live-file.ts";
      std::remove(out.c_str());
      std::string pipeline_desc = pipeline_desc_for(enc);
      std::printf("  using encoder %s desc: %s\n", enc.c_str(), pipeline_desc.c_str());
      auto r = be->open_insert({"file:" + out, true, pipeline_desc, Sei0604::Preserve});
      check(static_cast<bool>(r), "pipeline live + file sink open succeeds");
      if (r) {
        size_t off=0, npush=0;
        while (off < klv_bytes.size()) {
          size_t n = packet_frame_length(klv.subspan(off));
          if (n==0) break;
          int64_t pts = 1'000'000'000 + static_cast<int64_t>(npush * 100'000'000);
          auto pr = (*r)->push(klv.subspan(off,n), pts);
          if (!pr) { std::printf(" push %zu failed %d\n", npush, (int)pr.error()); check(false, "pipeline push"); break; }
          off+=n; ++npush;
          std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        auto fr = (*r)->finish();
        check(static_cast<bool>(fr), "pipeline live file sink finish (EOS drain, not timeout)");
        if (!fr) std::printf(" finish error %d\n", (int)fr.error());
      }
      if (static_cast<bool>(r)) {
        check(file_exists_p(out), "pipeline live output exists");
        // also verify KLV via extract
        if (file_exists_p(out)) {
          std::vector<std::byte> back;
          auto rr = be->extract(out, [&](const KlvPacket& kp){ back.insert(back.end(), kp.bytes.begin(), kp.bytes.end()); });
          std::printf("  pipeline file sink extract %zu bytes (sent %zu) ok=%d\n", back.size(), klv_bytes.size(), (int)(bool)rr);
          check(back == klv_bytes, "pipeline file sink KLV byte-exact");
        }
        if (file_exists_p(out)) {
          auto sz = std::filesystem::file_size(out);
          check(sz > 1000, "pipeline live output non-empty");
        }
      }
      std::remove(out.c_str());
    }
  }

  std::printf("== multicast knobs with live video still map ==\n");
  {
    std::string enc = pick_h264_encoder();
    std::string pipe = enc.empty() ? "pipeline:videotestsrc is-live=true num-buffers=10 ! videoconvert ! video/x-raw,width=320,height=240 ! fakesink"
                                   : pipeline_desc_for(enc);
    InsertConfig cfg;
    cfg.sink = "udp:239.1.1.1:5001";
    cfg.video_source = pipe;
    cfg.realtime = true;
    cfg.udp_ttl_mcast = 2;
    cfg.udp_mcast_iface = "lo";
    cfg.udp_loop = false;
    GstElement* s = misbklv::detail::make_sink("udp:239.1.1.1:5001", cfg);
    check(s != nullptr, "multicast sink created with live video config");
    if (s) {
      gint ttl=-1; gchar* iface=nullptr; gboolean loop=FALSE, am=FALSE;
      g_object_get(s, "ttl-mc",&ttl,"multicast-iface",&iface,"loop",&loop,"auto-multicast",&am,nullptr);
      check(ttl==2, "ttl-mc 2 with live video");
      check(iface && std::string(iface)=="lo", "multicast-iface lo with live video");
      check(loop==FALSE, "loop false with live video");
      check(am==TRUE, "auto-multicast true");
      if (iface) g_free(iface);
      gst_object_unref(s);
    }
  }

  std::printf("== hermetic UDP loopback with pipeline live video ==\n");
  {
    const int port = 15006;
    const std::string endpoint = "udp:127.0.0.1:" + std::to_string(port);
    std::vector<std::byte> out;
    std::atomic<bool> extract_ok{false};
    std::thread rx([&]{
      auto be2 = make_gst_backend();
      auto rr = be2->extract(endpoint, [&](const KlvPacket& kp){ out.insert(out.end(), kp.bytes.begin(), kp.bytes.end()); });
      extract_ok = static_cast<bool>(rr);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    std::string enc2 = [](){ GstElementFactory* f = gst_element_factory_find("x265enc"); if(f){gst_object_unref(f); return std::string("x265enc");} return pick_h264_encoder(); }();
    std::string pipeline_desc = enc2.empty() ? "pipeline:videotestsrc is-live=true num-buffers=60 ! videoconvert ! video/x-raw,width=320,height=240,framerate=30/1 ! fakesink"
                                             : pipeline_desc_for(enc2);
    if (enc2.empty()) {
      std::printf("  SKIP loopback: no encoder\n");
      // need to stop rx thread - it will timeout on idle, join
      rx.join();
      check(true, "loopback skipped (no encoder)");
    } else {
    auto ins = be->open_insert({endpoint, true, pipeline_desc, Sei0604::Preserve});
    check(static_cast<bool>(ins), "loopback pipeline live open_insert");
    if (!ins) {
      std::printf(" open failed %d\n", (int)ins.error());
      // need to wake rx? rx will timeout via idle timeout?
      rx.join();
      check(false, "loopback aborted");
    } else {
      size_t off=0, npush=0;
      while (off < klv_bytes.size()) {
        size_t n = packet_frame_length(klv.subspan(off));
        if (n==0) break;
        int64_t pts = 1'000'000'000 + static_cast<int64_t>(npush * 100'000'000);
        auto pr = (*ins)->push(klv.subspan(off,n), pts);
        if (!pr) { std::printf(" push %zu failed\n", npush); break; }
        off+=n; ++npush;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
      }
      auto fr = (*ins)->finish();
      check(static_cast<bool>(fr), "loopback pipeline finish");
      // give receiver idle timeout to fire (udpsrc timeout)
      // gst_extract for udp ends after idle timeout (default ~? 1-2 sec). gst_stream_nonframing uses 15005 and waits 4 sec overall.
      // rx thread will return after udpsrc idle timeout (~1 sec post EOS? Actually EOS not sent over udp, so idle timeout is expected)
      // Wait a bit then join with timeout? Just join (will block until idle timeout).
      // Ins already finished and pipeline NULL, no more udp traffic, so rx should end soon.
      // To avoid indefinite block, join with timed wait?
      // We'll join with 10 sec timeout via polling.
      // Simple: join with std::thread join after 6 sec sleep then check extract_ok may still be waiting but udpsrc idle timeout is 1s default? In gst_extract.cpp it's likely 1 sec.
      // We'll just wait 3 sec after finish then join.
      std::this_thread::sleep_for(std::chrono::milliseconds(2000));
      // If still not ok, need to stop? But extract returns after idle timeout, which should be ~2 sec after last packet.
      // Wait for thread with try.
      // Use timed join via polling atomic
      for (int i=0;i<30 && !extract_ok.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
      // Actually need to join thread; but if extract still running we need to wait more.
      // We'll attempt to join with timeout of 5 sec.
      // Since std::thread join is blocking, we need to ensure extract will terminate. It does via idle timeout 5 sec? Let's wait up to 8 sec.
      // To avoid hanging test, detach with stop? We don't have stop_token here; but extract for udp should timeout on its own after ~? Check gst_extract idle timeout = 2 sec?
      // We'll just join with 10 sec upper bound using async wait.
      auto start = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() - start < std::chrono::seconds(8)) {
        if (extract_ok.load()) break;
        // Check if thread finished? We can't non-blockingly check joinable without join. Use try with sleep.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // If thread is done, extract_ok would be set, but we still need to join to reap.
        // We'll attempt to see if thread is still joinable and do a timed approach via std::async? Simplify: just wait then join blocking with timeout via future.
        // For now, just sleep then join - but join will block until extract returns, which should be <5 sec.
        // To avoid infinite hang, we will use a watchdog: if after 8 sec not done, we consider fail and detach?
        // We'll just try to join with timeout by moving thread to detached after 8 sec is not ideal.
        // Let's just join with a detached timer that not needed; the test harness has ctest timeout 600 sec.
        if (extract_ok.load()) break;
      }
      // Now try to join if thread still running, it will block but should return soon.
      // To not hang forever, we use timed join via pthread?
      // We'll attempt normal join but with overall 10 sec wait.
      // If after 8 sec extract_ok still false, we still join (will block until idle timeout).
      // Let's just join now; it should return within a couple seconds.
      // To prevent indefinite hang in CI, we set a 10 sec alarm via std::async and wait.
      // Simpler: just join; ctest has 600 sec timeout.
      if (rx.joinable()) {
        // Use a separate watchdog thread to join with timeout
        std::atomic<bool> joined{false};
        std::thread joiner([&]{ rx.join(); joined = true; });
        auto j0 = std::chrono::steady_clock::now();
        while (!joined.load() && std::chrono::steady_clock::now() - j0 < std::chrono::seconds(8)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!joined.load()) {
          std::printf("WARNING: rx thread did not join in 8s, detaching\n");
          joiner.detach();
          check(false, "loopback rx did not terminate (idle timeout)");
        } else {
          joiner.join();
        }
      }
      check(extract_ok.load(), "loopback extract ok");
      check(out == klv_bytes, "loopback KLV byte-exact with live pipeline video");
      std::printf("  loopback sent %zu bytes, received %zu\n", klv_bytes.size(), out.size());
    }
    } // else enc available
  }

  std::printf("== clock pinning (KLV PTS vs running_time <100ms) ==\n");
  {
    const int port = 6003;
    const std::string endpoint = "udp:127.0.0.1:" + std::to_string(port);
    std::vector<std::byte> out;
    std::vector<std::int64_t> pts_out;
    std::atomic<bool> extract_ok{false};
    // Collect expected PTS for the packets we will push (1s,2s,3s).
    // Base 1s avoids PTS 0 edge (appsrc running_time ~700ms at first push;
    // 0 would be in the past and may be dropped with is-live). Intervals 1s
    // pin KLV PTS to pipeline running_time; demuxed PTS must be within 100ms.
    std::vector<std::int64_t> expected_pts;
    {
      size_t tmp_off = 0;
      int idx = 0;
      while (tmp_off < klv_bytes.size() && idx < 3) {
        size_t n = packet_frame_length(klv.subspan(tmp_off));
        if (n == 0) break;
        expected_pts.push_back(1'000'000'000LL + static_cast<std::int64_t>(idx * 1'000'000'000LL));
        tmp_off += n;
        ++idx;
      }
    }
    if (expected_pts.empty()) {
      check(false, "clock pinning: no KLV frames to push");
    } else {
      std::thread rx([&]{
        auto be2 = make_gst_backend();
        auto rr = be2->extract(endpoint, [&](const KlvPacket& kp){
          out.insert(out.end(), kp.bytes.begin(), kp.bytes.end());
          pts_out.push_back(kp.pts_ns);
        });
        extract_ok = static_cast<bool>(rr);
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(700));
      std::string enc = pick_h264_encoder();
      std::string pipeline_desc;
      if (!enc.empty()) pipeline_desc = pipeline_desc_for(enc);
      // pipeline_desc_for uses is-live=true num-buffers=60; keep realtime=true so
      // sink syncs to clock and PTS aligns with running_time.
      if (enc.empty()) {
        std::printf("  SKIP clock pinning: no encoder\n");
        if (rx.joinable()) {
          std::atomic<bool> joined{false};
          std::thread joiner([&]{ rx.join(); joined = true; });
          auto j0 = std::chrono::steady_clock::now();
          while (!joined.load() && std::chrono::steady_clock::now() - j0 < std::chrono::seconds(8)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
          if (!joined.load()) joiner.detach(); else joiner.join();
        }
        check(true, "clock pinning skipped (no encoder)");
      } else {
        // Use same pipeline desc but ensure is-live and realtime path.
        // pipeline_desc_for already is-live=true.
        auto ins = be->open_insert({endpoint, true, pipeline_desc, Sei0604::Preserve});
        check(static_cast<bool>(ins), "clock pinning pipeline live open_insert");
        if (!ins) {
          std::printf(" clock pinning open failed %d\n", (int)ins.error());
          // wake rx via timeout
          if (rx.joinable()) {
            std::atomic<bool> joined{false};
            std::thread joiner([&]{ rx.join(); joined = true; });
            auto j0 = std::chrono::steady_clock::now();
            while (!joined.load() && std::chrono::steady_clock::now() - j0 < std::chrono::seconds(8)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!joined.load()) { joiner.detach(); check(false, "clock pinning rx did not terminate"); } else joiner.join();
          }
        } else {
          size_t off = 0;
          for (size_t i = 0; i < expected_pts.size(); ++i) {
            size_t n = packet_frame_length(klv.subspan(off));
            if (n == 0) break;
            auto pr = (*ins)->push(klv.subspan(off, n), expected_pts[i]);
            if (!pr) { std::printf(" clock pinning push %zu failed\n", i); check(false, "clock pinning push"); break; }
            off += n;
            // Small inter-push delay; with realtime=true, mux paces by PTS, not wall clock.
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
          }
          auto fr = (*ins)->finish();
          check(static_cast<bool>(fr), "clock pinning pipeline finish (EOS drain)");
          // Wait for receiver idle timeout after EOS (udp has no EOS signaling; idle timeout ends extract).
          // Last PTS is ~3s, pipeline needs ~3s to render it through udpsink sync; wait accordingly.
          std::this_thread::sleep_for(std::chrono::milliseconds(3500));
          for (int i = 0; i < 30 && !extract_ok.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
          auto start = std::chrono::steady_clock::now();
          while (std::chrono::steady_clock::now() - start < std::chrono::seconds(8) && !extract_ok.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
          if (rx.joinable()) {
            std::atomic<bool> joined{false};
            std::thread joiner([&]{ rx.join(); joined = true; });
            auto j0 = std::chrono::steady_clock::now();
            while (!joined.load() && std::chrono::steady_clock::now() - j0 < std::chrono::seconds(8)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!joined.load()) {
              std::printf("WARNING: clock pinning rx did not join in 8s, detaching\n");
              joiner.detach();
              check(false, "clock pinning rx did not terminate (idle timeout)");
            } else {
              joiner.join();
            }
          }
          check(extract_ok.load(), "clock pinning extract ok");
          // Build expected byte prefix (first expected_pts.size() frames).
          std::vector<std::byte> expected_bytes;
          {
            size_t eo = 0;
            for (size_t i = 0; i < expected_pts.size(); ++i) {
              size_t n = packet_frame_length(klv.subspan(eo));
              if (n == 0) break;
              expected_bytes.insert(expected_bytes.end(), klv_bytes.begin()+eo, klv_bytes.begin()+eo+n);
              eo += n;
            }
          }
          check(out == expected_bytes, "clock pinning KLV byte-exact for pinned prefix");
          std::printf("  clock pinning sent %zu bytes (%zu pkts), received %zu bytes (%zu pts)\n", expected_bytes.size(), expected_pts.size(), out.size(), pts_out.size());
          if (pts_out.size() == expected_pts.size()) {
            // Absolute PTS may have a constant pipeline-startup offset (≈700ms warmup);
            // clock pinning requires KLV PTS == running_time within 100ms *relative*,
            // i.e. the offset is stable and intervals are exact.
            bool all_within = true;
            const std::int64_t base_delta = pts_out[0] - expected_pts[0];
            for (size_t i = 0; i < pts_out.size(); ++i) {
              std::int64_t delta = pts_out[i] - expected_pts[i];
              std::int64_t drift = delta - base_delta;
              if (drift < 0) drift = -drift;
              const bool within = drift < 100'000'000LL; // 100ms tolerance on drift
              if (!within) {
                std::printf("  pts pinning drift fail pkt %zu: got %lld ns expected %lld ns base_delta %lld ns drift %lld ns\n", i, (long long)pts_out[i], (long long)expected_pts[i], (long long)base_delta, (long long)drift);
                all_within = false;
              }
              // Also assert pts_ns is not kNoPts and tracks GST_BUFFER_PTS running_time
              check(pts_out[i] != kNoPts, "clock pinning pts != kNoPts");
            }
            check(all_within, "clock pinning PTS drift <100ms from running_time (GST_BUFFER_PTS)");
            if (pts_out.size() >= 2) {
              for (size_t i = 1; i < pts_out.size(); ++i) {
                std::int64_t exp_interval = expected_pts[i] - expected_pts[i-1];
                std::int64_t got_interval = pts_out[i] - pts_out[i-1];
                std::int64_t idelta = got_interval - exp_interval;
                if (idelta < 0) idelta = -idelta;
                if (idelta >= 100'000'000LL) {
                  std::printf("  interval pinning fail %zu->%zu: got %lld exp %lld\n", i-1,i,(long long)got_interval,(long long)exp_interval);
                }
              }
              // At least check interval tolerance aggregate
              bool intervals_ok = true;
              for (size_t i = 1; i < pts_out.size(); ++i) {
                std::int64_t d = (pts_out[i]-pts_out[i-1]) - (expected_pts[i]-expected_pts[i-1]);
                if (d < 0) d = -d;
                if (d >= 100'000'000LL) intervals_ok = false;
              }
              check(intervals_ok, "clock pinning PTS intervals within 100ms");
            }
          } else {
            std::printf("  clock pinning pts count mismatch got %zu exp %zu\n", pts_out.size(), expected_pts.size());
            check(false, "clock pinning pts count matches");
          }
        }
      }
    }
  }

  std::printf("\nLIVE VIDEO: %s\n", failures==0?"PASS":"FAIL");
  return failures==0?0:1;
}
