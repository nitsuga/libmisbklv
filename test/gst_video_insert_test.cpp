// SPDX-License-Identifier: Apache-2.0
// Video passthrough on the insert path (ADR 0020): filesrc ! parsebin joins
// appsrc(meta/x-klv) ! mpegtsmux ! filesink, so one open_insert() writes a .ts
// carrying BOTH a video PID and a KLV PID. Asserts, over the muxed output:
//   1. the PMT lists two elementary streams — video + KLV (0x06 + "KLVA")
//   2. the KLV comes back byte-exact (the gst_insert_test property, with video)
//   3. the video elementary stream is passthrough, not re-encoded
//   4. the KLV PES timestamps are the ones we pushed (90 kHz rounding)
//   5. the source's own KLV / audio streams are dropped, not forwarded
//   6. a failed open_insert() writes no output file — and never deletes one it
//      did not create
//   7. BOTH library extractors read those timestamps back (ADR 0021) — the gst
//      backend and the gst-free extract_ts_klv — so the write side's timeline is
//      the read side's timeline, checked against the PES parser below
//   8. read -> edit -> write composes (ADR 0021): a KlvStream of the output,
//      re-emitted through a KlvSink with the same video, keeps its timing
//   9. no output file after a failure LATER than open_insert either (ADR 0022) —
//      a failing finish(), and a session abandoned without one
// Runs the whole battery twice: once on the given MPEG-TS source, then again on
// an MP4 remuxed from it (the `qtdemux` path — what a consumer converting MP4s
// actually hits, and a different demuxer + caps negotiation into the muxer).
// A non-TS source skips the source-comparison checks, which need a TS to read.
// argv: <video-source.ts> <input.klv> <temp.ts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <gst/gst.h>

#include "misbklv/backend.hpp"
#include "misbklv/gst_backend.hpp"
#include "misbklv/message.hpp"
#include "misbklv/packet.hpp"
#include "misbklv/stream.hpp"
#include "misbklv/ts.hpp"

using namespace misbklv;

static std::vector<std::byte> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i)
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}

static bool file_exists(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return f.good();
}

// --- a minimal MPEG-TS reader, just enough to check what we muxed ------------
// Deliberately independent of src/ts.cpp: this test is about the *container* the
// muxer produced (PMT contents, per-PID PES payloads and their PTS), not about
// KLV framing, so it re-reads the TS rather than trusting our own extractor.
namespace {

constexpr std::size_t kPkt = 188;

struct EsInfo {
  unsigned pid = 0;
  unsigned stream_type = 0;
  bool klva = false;  // registration descriptor "KLVA"
};

struct Pes {
  std::vector<std::byte> payload;      // concatenated PES payloads for one PID
  std::vector<std::int64_t> pts_90k;   // one per PES packet that carried a PTS
};

inline unsigned u8(std::span<const std::byte> b, std::size_t i) {
  return static_cast<unsigned>(b[i]);
}

// Two sync bytes one packet apart — enough to tell an MPEG-TS from an MP4.
bool is_mpegts(std::span<const std::byte> b) {
  return b.size() > kPkt && u8(b, 0) == 0x47 && u8(b, kPkt) == 0x47;
}

// Parse the first PAT + PMT and return the program's elementary streams.
std::vector<EsInfo> read_pmt(std::span<const std::byte> ts) {
  std::vector<EsInfo> out;
  unsigned pmt_pid = 0;
  for (std::size_t off = 0; off + kPkt <= ts.size(); off += kPkt) {
    auto p = ts.subspan(off, kPkt);
    if (u8(p, 0) != 0x47) continue;
    const bool pusi = (u8(p, 1) & 0x40) != 0;
    const unsigned pid = ((u8(p, 1) & 0x1f) << 8) | u8(p, 2);
    const unsigned afc = (u8(p, 3) >> 4) & 0x3;
    if (!(afc & 0x1) || !pusi) continue;
    std::size_t i = 4;
    if (afc & 0x2) i += 1 + u8(p, 4);
    if (i >= kPkt) continue;
    i += 1 + u8(p, i);  // pointer_field
    if (i + 8 >= kPkt) continue;
    const unsigned table_id = u8(p, i);
    const std::size_t seclen = ((u8(p, i + 1) & 0x0f) << 8) | u8(p, i + 2);
    const std::size_t end = i + 3 + seclen - 4;  // less CRC32
    if (end > kPkt) continue;                    // multi-packet section: skip
    if (table_id == 0x00 && pmt_pid == 0) {      // PAT
      for (std::size_t j = i + 8; j + 4 <= end; j += 4) {
        const unsigned prog = (u8(p, j) << 8) | u8(p, j + 1);
        if (prog != 0) {  // 0 = network PID
          pmt_pid = ((u8(p, j + 2) & 0x1f) << 8) | u8(p, j + 3);
          break;
        }
      }
    } else if (table_id == 0x02 && pid == pmt_pid && out.empty()) {  // PMT
      std::size_t j = i + 8 + 2;  // skip PCR_PID
      const std::size_t pil = ((u8(p, j) & 0x0f) << 8) | u8(p, j + 1);
      j += 2 + pil;
      while (j + 5 <= end) {
        EsInfo es;
        es.stream_type = u8(p, j);
        es.pid = ((u8(p, j + 1) & 0x1f) << 8) | u8(p, j + 2);
        const std::size_t esil = ((u8(p, j + 3) & 0x0f) << 8) | u8(p, j + 4);
        for (std::size_t d = j + 5; d + 2 <= j + 5 + esil;) {  // descriptors
          const unsigned tag = u8(p, d), len = u8(p, d + 1);
          if (tag == 0x05 && len >= 4 && u8(p, d + 2) == 'K' &&
              u8(p, d + 3) == 'L' && u8(p, d + 4) == 'V' && u8(p, d + 5) == 'A')
            es.klva = true;
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

std::int64_t read_pts(std::span<const std::byte> pes) {  // -1 if absent
  if (pes.size() < 14 || !(u8(pes, 7) & 0x80)) return -1;
  return (static_cast<std::int64_t>(u8(pes, 9) & 0x0e) << 29) |
         (static_cast<std::int64_t>(u8(pes, 10)) << 22) |
         (static_cast<std::int64_t>(u8(pes, 11) & 0xfe) << 14) |
         (static_cast<std::int64_t>(u8(pes, 12)) << 7) |
         (static_cast<std::int64_t>(u8(pes, 13)) >> 1);
}

// Reassemble one PID's PES packets: elementary-stream bytes + per-PES PTS.
Pes read_pes(std::span<const std::byte> ts, unsigned want_pid) {
  Pes out;
  std::vector<std::byte> cur;
  auto flush = [&] {
    if (cur.size() < 9) return;
    const std::int64_t pts = read_pts(cur);
    if (pts >= 0) out.pts_90k.push_back(pts);
    const std::size_t hdr = 9 + u8(cur, 8);
    if (hdr <= cur.size())
      out.payload.insert(out.payload.end(), cur.begin() + hdr, cur.end());
    cur.clear();
  };
  for (std::size_t off = 0; off + kPkt <= ts.size(); off += kPkt) {
    auto p = ts.subspan(off, kPkt);
    if (u8(p, 0) != 0x47) continue;
    const unsigned pid = ((u8(p, 1) & 0x1f) << 8) | u8(p, 2);
    if (pid != want_pid) continue;
    const unsigned afc = (u8(p, 3) >> 4) & 0x3;
    if (!(afc & 0x1)) continue;
    std::size_t i = 4;
    if (afc & 0x2) i += 1 + u8(p, 4);
    if (i >= kPkt) continue;
    if (u8(p, 1) & 0x40) flush();  // payload_unit_start: previous PES is done
    cur.insert(cur.end(), p.begin() + i, p.end());
  }
  flush();
  return out;
}

bool is_video(unsigned stream_type) {  // mpeg2, h264, h265
  return stream_type == 0x02 || stream_type == 0x1b || stream_type == 0x24;
}

// Compare a library extractor's per-packet timestamps against the ones pushed.
// Both sides are "nanoseconds from the start of the source" (ADR 0021), but the
// origin is the muxer's stream start rather than ours, so what must match is
// every interval from the first packet — and no packet may come back kNoPts,
// which is the whole defect this guards. Tolerance: the values made a round trip
// through the 90 kHz PES grid, so allow two ticks.
bool pts_series_ok(const char* who, const std::vector<std::int64_t>& got,
                   const std::vector<std::int64_t>& want) {
  constexpr std::int64_t kTol = 25'000;  // ns; one 90 kHz tick is 11 111 ns
  if (got.size() != want.size()) {
    std::printf("  %s: %zu timestamps for %zu packets\n", who, got.size(),
                want.size());
    return false;
  }
  for (std::size_t i = 0; i < got.size(); ++i) {
    if (got[i] == kNoPts) {
      std::printf("  %s: packet %zu came back kNoPts\n", who, i);
      return false;
    }
    const std::int64_t diff = (got[i] - got[0]) - (want[i] - want[0]);
    if (std::llabs(diff) > kTol) {
      std::printf("  %s: packet %zu off by %lld ns\n", who, i,
                  static_cast<long long>(diff));
      return false;
    }
  }
  return true;
}

bool g_pass = true;
void check(const char* what, bool ok) {
  std::printf("  %-28s %s\n", what, ok ? "PASS" : "FAIL");
  g_pass = g_pass && ok;
}

// Remux a TS's video into an MP4, so the qtdemux path gets covered without
// committing a binary fixture or needing an encoder. Returns false (and the case
// is skipped) if the mp4 muxer isn't in this gstreamer install.
bool remux_to_mp4(const std::string& ts, const std::string& mp4) {
  gst_init(nullptr, nullptr);
  GstElementFactory* f = gst_element_factory_find("mp4mux");
  if (!f) return false;
  gst_object_unref(f);
  const std::string desc = "filesrc location=\"" + ts +
                           "\" ! tsdemux ! h264parse ! mp4mux ! filesink "
                           "location=\"" + mp4 + "\"";
  GError* err = nullptr;
  GstElement* p = gst_parse_launch(desc.c_str(), &err);
  if (err) { g_error_free(err); }
  if (!p) return false;
  bool ok = gst_element_set_state(p, GST_STATE_PLAYING) !=
            GST_STATE_CHANGE_FAILURE;
  if (ok) {
    GstBus* bus = gst_element_get_bus(p);
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, 30 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    ok = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (msg) gst_message_unref(msg);
    gst_object_unref(bus);
  }
  gst_element_set_state(p, GST_STATE_NULL);
  gst_object_unref(p);
  return ok && file_exists(mp4);
}

// Mux `source`'s video + `input`'s KLV into `out_path` and check the result.
// Returns the number of video PES in the output (0 on a hard failure), so a
// later case can be compared against an earlier one.
std::size_t run_case(MediaBackend& be, const std::string& source,
                     const std::vector<std::byte>& input,
                     const std::string& out_path) {
  std::span<const std::byte> buf = input;
  // KLV 100 ms apart, on the source's timeline from zero — the contract a video
  // branch imposes on the caller (kNoPts is rejected; checked below).
  constexpr std::int64_t kStepNs = 100'000'000;
  std::vector<std::int64_t> pushed;
  {
    auto ins = be.open_insert({"file:" + out_path, false, source});
    if (!ins) {
      std::printf("  open_insert failed: %d\n", static_cast<int>(ins.error()));
      g_pass = false;
      return 0;
    }
    check("kNoPts rejected",
          !(*ins)->push(buf.subspan(0, packet_frame_length(buf)), kNoPts));
    std::size_t off = 0;
    while (off < input.size()) {
      const std::size_t n = packet_frame_length(buf.subspan(off));
      if (n == 0) break;
      const std::int64_t pts = static_cast<std::int64_t>(pushed.size()) * kStepNs;
      if (!(*ins)->push(buf.subspan(off, n), pts)) {
        std::printf("  push failed at packet %zu\n", pushed.size());
        g_pass = false;
        return 0;
      }
      pushed.push_back(pts);
      off += n;
    }
    if (!(*ins)->finish()) {
      std::printf("  finish failed\n");
      g_pass = false;
      return 0;
    }
  }
  const auto out_ts = read_file(out_path.c_str());
  std::printf("  muxed %zu KLV packets + video -> %zu bytes\n", pushed.size(),
              out_ts.size());

  // --- 1. the PMT lists exactly one video ES and one KLV ES ------------------
  const auto es = read_pmt(out_ts);
  unsigned video_pid = 0, video_type = 0, nvideo = 0, nklv = 0, klv_pid = 0;
  for (const auto& e : es) {
    std::printf("  ES pid=0x%04x stream_type=0x%02x%s\n", e.pid, e.stream_type,
                e.klva ? " KLVA" : "");
    if (is_video(e.stream_type) && !video_pid) {
      video_pid = e.pid;
      video_type = e.stream_type;
    }
    if (is_video(e.stream_type)) ++nvideo;
    if (e.stream_type == 0x06 && e.klva) { ++nklv; klv_pid = e.pid; }
  }
  check("PMT: video + KLV, only", es.size() == 2 && nvideo == 1 && nklv == 1);

  // --- 2. KLV byte-exact through the mux ------------------------------------
  std::vector<std::byte> klv_back;
  std::vector<std::int64_t> gst_pts;
  auto r = be.extract(out_path, [&](const KlvPacket& kp) {
    klv_back.insert(klv_back.end(), kp.bytes.begin(), kp.bytes.end());
    gst_pts.push_back(kp.pts_ns);
  });
  check("KLV byte-exact", r && klv_back == input);

  // --- 3. video is passthrough, not re-encoded ------------------------------
  const auto out_video = read_pes(out_ts, video_pid);
  const auto src_bytes = read_file(source.c_str());
  if (is_mpegts(src_bytes)) {  // compare against the source's own video ES
    unsigned src_video_pid = 0, src_video_type = 0;
    for (const auto& e : read_pmt(src_bytes))
      if (is_video(e.stream_type) && !src_video_pid) {
        src_video_pid = e.pid;
        src_video_type = e.stream_type;
      }
    const auto src_video = read_pes(src_bytes, src_video_pid);
    std::printf("  video ES: source %zu bytes / %zu PES, output %zu / %zu\n",
                src_video.payload.size(), src_video.pts_90k.size(),
                out_video.payload.size(), out_video.pts_90k.size());
    check("video codec unchanged", video_type == src_video_type && video_type);
    // Fork 21: video passthrough now generates ST 0604 SEI per frame (~35 bytes each),
    // so output ES is larger. Check that output >= source (not byte-exact).
    check("video ES preserved (with SEI)", out_video.payload.size() >= src_video.payload.size());
    check("video frame count kept",
          out_video.pts_90k.size() == src_video.pts_90k.size());
  } else {
    // Not an MPEG-TS: this reader can't pull the source's elementary stream, so
    // the source-comparison checks are skipped rather than reported as failures
    // (the muxing is still fully checked — PMT, KLV, PTS, and the frame count
    // against the TS case by the caller).
    std::printf("  video ES: output %zu bytes / %zu PES"
                " (source is not MPEG-TS — comparison skipped)\n",
                out_video.payload.size(), out_video.pts_90k.size());
    check("video carried", !out_video.payload.empty());
  }

  // --- 4. KLV PTS survive the mux, on the video's timeline ------------------
  // mpegtsmux maps running time onto the TS clock with a fixed offset (it starts
  // the stream an hour in so that early timestamps can't go negative), so the
  // absolute 90 kHz values are not the pushed nanoseconds. What must hold is
  // that (a) the *intervals* we pushed are preserved exactly, and (b) the KLV
  // timeline shares its origin with the video's — that shared origin is the
  // whole point of the PTS contract.
  const auto out_klv = read_pes(out_ts, klv_pid);
  auto min_pts = [](const std::vector<std::int64_t>& v) {  // video PTS may reorder
    std::int64_t m = v.empty() ? 0 : v[0];
    for (auto x : v) m = (x < m) ? x : m;
    return m;
  };
  const std::int64_t base = min_pts(out_klv.pts_90k);
  const std::int64_t vbase = min_pts(out_video.pts_90k);
  bool pts_ok = out_klv.pts_90k.size() == pushed.size();
  for (std::size_t i = 0; pts_ok && i < pushed.size(); ++i) {
    const std::int64_t want = (pushed[i] - pushed[0]) * 9 / 100'000;  // ns->90kHz
    const std::int64_t got = out_klv.pts_90k[i] - base;
    if (std::llabs(got - want) > 1) {  // muxer rounding
      std::printf("  pts[%zu]: want %lld got %lld\n", i,
                  static_cast<long long>(want), static_cast<long long>(got));
      pts_ok = false;
    }
  }
  check("KLV PTS preserved", pts_ok);
  // Both branches ran from zero, so their origins must coincide within a frame.
  check("KLV shares video timeline", std::llabs(base - vbase) <= 90000 / 20);

  // --- 7. and the library reads those timestamps back (ADR 0021) ------------
  // Same file, same expectation, but through the two extractors a consumer
  // actually calls — the PES parser above is only the independent witness that
  // the timing is in the file at all. Before ADR 0021 both reported kNoPts here.
  check("gst extract reports PTS", pts_series_ok("gst extract", gst_pts, pushed));
  std::vector<std::int64_t> ts_pts;
  auto tr = extract_ts_klv(out_ts, [&](const KlvPacket& kp) {
    ts_pts.push_back(kp.pts_ns);
  });
  check("extract_ts_klv reports PTS",
        tr && pts_series_ok("extract_ts_klv", ts_pts, pushed));

  // --- 8. read -> edit -> write composes (ADR 0021) -------------------------
  // The facade's two halves over one timeline: read the file we just wrote as
  // Messages, emit them into a NEW sink carrying the same video, and check the
  // timing survived. This is the round trip that a kNoPts read path made
  // impossible — KlvSink rejects an unset PTS when there is a video branch, so
  // before the fix this did not merely drift, it failed outright.
  const std::string round_path = out_path + ".roundtrip.ts";
  std::remove(round_path.c_str());
  std::size_t emitted = 0;
  bool emit_ok = true;
  {
    KlvSink out(make_gst_backend(), "file:" + round_path, false, source);
    KlvStream in(make_gst_backend(), out_path);
    for (Message& m : in) {
      if (!out.emit(m)) { emit_ok = false; break; }
      ++emitted;
    }
    emit_ok = emit_ok && out.close();
  }
  check("round trip: re-emitted", emit_ok && emitted == pushed.size());
  if (emit_ok) {
    std::vector<std::int64_t> round_pts;
    auto rr = be.extract(round_path, [&](const KlvPacket& kp) {
      round_pts.push_back(kp.pts_ns);
    });
    check("round trip: timing kept",
          rr && pts_series_ok("round trip", round_pts, pushed));
  }
  std::remove(round_path.c_str());
  return out_video.pts_90k.size();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: gst_video_insert_test <video.ts> <input.klv> <out.ts>\n");
    return 2;
  }
  const std::string ts_source = argv[1];
  const auto input = read_file(argv[2]);
  const std::string out_path = argv[3];
  auto be = make_gst_backend();

  // An MP4 remuxed from the TS source: the qtdemux path for the main battery,
  // and the raw material for the late-failure case below. Absent if this
  // gstreamer has no usable mp4mux, in which case both are skipped.
  const std::string mp4 = out_path + ".src.mp4";
  std::remove(mp4.c_str());
  const bool have_mp4 = remux_to_mp4(ts_source, mp4);

  // --- 6/9. no output file after a failure, wherever it surfaces ------------
  std::printf("failure paths\n");
  {
    // (a) missing source — rejected by the pre-flight check, before any element
    //     exists, so nothing can have been created.
    const std::string p = out_path + ".missing.ts";
    std::remove(p.c_str());
    auto r = be->open_insert({"file:" + p, false, ts_source + ".does-not-exist"});
    check("missing source: rejected", !r);
    check("missing source: no output", !file_exists(p));
    std::remove(p.c_str());
  }
  {
    // (b) a source that opens fine but has no video stream. This one only fails
    //     in PAUSED — by which point the file sink has created its file — so it
    //     is the case that proves the cleanup, not just the pre-flight check.
    const std::string p = out_path + ".videoless.ts";
    std::remove(p.c_str());
    auto r = be->open_insert({"file:" + p, false, argv[2]});  // the .klv itself
    check("videoless source: rejected", !r);
    check("videoless source: no output", !file_exists(p));
    std::remove(p.c_str());
  }
  {
    // (c) ...but a file that was already there is the caller's, not ours to
    //     delete: the cleanup must remove only what this call created. (The
    //     file sink truncates whatever it opens, on the success path too, so
    //     what's guaranteed here is that the path still exists — not that its
    //     old contents survive a failed open.)
    const std::string p = out_path + ".keepme.ts";
    { std::ofstream f(p, std::ios::binary); f << "not ours to delete"; }
    auto r = be->open_insert({"file:" + p, false, argv[2]});
    check("pre-existing output not deleted", !r && file_exists(p));
    std::remove(p.c_str());
  }
  {
    // (d) a session abandoned without finish(): the pipeline ran, the sink file
    //     exists, but nothing finalized it. An unfinished .ts is not output
    //     either, so the guarantee covers the whole session, not just the open
    //     (ADR 0022).
    const std::string p = out_path + ".abandoned.ts";
    std::remove(p.c_str());
    auto ins = be->open_insert({"file:" + p, false, ts_source});
    check("abandoned: opened", static_cast<bool>(ins));
    if (ins) {
      std::span<const std::byte> buf = input;
      (*ins)->push(buf.subspan(0, packet_frame_length(buf)), 0);
      ins->reset();  // destroy without finish()
      check("abandoned: no output", !file_exists(p));
    }
    std::remove(p.c_str());
  }
  if (have_mp4) {
    // (e) the finding this case exists for: a source whose video track is
    //     *declared but unparseable*. Blanking the avcC box leaves qtdemux an
    //     avc1 sample entry with no codec_data — a video pad appears, so
    //     open_insert succeeds and pushes are accepted, and the failure only
    //     surfaces when finish() drains the pipeline. Before ADR 0022 that left
    //     a zero-byte .ts behind, which reads as output to anything scanning
    //     the directory.
    auto bytes = read_file(mp4.c_str());
    std::size_t at = std::string::npos;
    for (std::size_t i = 0; i + 4 <= bytes.size(); ++i)
      if (u8(bytes, i) == 'a' && u8(bytes, i + 1) == 'v' &&
          u8(bytes, i + 2) == 'c' && u8(bytes, i + 3) == 'C') { at = i; break; }
    if (at == std::string::npos) {
      std::printf("  late failure: SKIPPED (no avcC box in the remuxed MP4)\n");
    } else {
      const char kFree[] = "free";  // same size, so the box layout still walks
      for (std::size_t i = 0; i < 4; ++i)
        bytes[at + i] = static_cast<std::byte>(kFree[i]);
      const std::string broken = out_path + ".no-avcc.mp4";
      { std::ofstream f(broken, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }

      const std::string p = out_path + ".late.ts";
      std::remove(p.c_str());
      auto ins = be->open_insert({"file:" + p, false, broken});
      check("late failure: opens", static_cast<bool>(ins));
      if (ins) {
        std::span<const std::byte> buf = input;
        (*ins)->push(buf.subspan(0, packet_frame_length(buf)), 0);
        check("late failure: finish fails", !(*ins)->finish());
        ins->reset();
        check("late failure: no output", !file_exists(p));
      }
      std::remove(p.c_str());
      // ...and the same case must still not delete a file it did not create.
      const std::string q = out_path + ".late-keepme.ts";
      { std::ofstream f(q, std::ios::binary); f << "not ours to delete"; }
      auto ins2 = be->open_insert({"file:" + q, false, broken});
      if (ins2) {
        (*ins2)->finish();
        ins2->reset();
      }
      check("late failure: pre-existing output kept", file_exists(q));
      std::remove(q.c_str());
      std::remove(broken.c_str());
    }
  } else {
    std::printf("  late failure: SKIPPED (needs the remuxed MP4)\n");
  }

  // --- the full battery, on the TS source and then on an MP4 remuxed from it -
  std::printf("MPEG-TS source (%s)\n", ts_source.c_str());
  const std::size_t ts_frames = run_case(*be, ts_source, input, out_path);

  if (have_mp4) {
    std::printf("MP4 source (qtdemux path, remuxed from the TS)\n");
    const std::size_t mp4_frames = run_case(*be, mp4, input, out_path + ".mp4.ts");
    check("MP4 path: same frame count", mp4_frames == ts_frames && ts_frames);
    std::remove(mp4.c_str());
    std::remove((out_path + ".mp4.ts").c_str());
  } else {
    std::printf("MP4 source: SKIPPED (no usable mp4mux in this gstreamer)\n");
  }

  std::printf("VIDEO PASSTHROUGH: %s\n", g_pass ? "PASS" : "FAIL");
  return g_pass ? 0 : 1;
}
