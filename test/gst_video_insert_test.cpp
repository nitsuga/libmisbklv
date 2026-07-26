// SPDX-License-Identifier: Apache-2.0
// Video passthrough on the insert path (ADR 0020): filesrc ! parsebin joins
// appsrc(meta/x-klv) ! mpegtsmux ! filesink, so one open_insert() writes a .ts
// carrying BOTH a video PID and a KLV PID. Asserts, over the muxed output:
//   1. the PMT lists two elementary streams — video + KLV (0x06 + "KLVA")
//   2. the KLV comes back byte-exact (the gst_insert_test property, with video)
//   3. the video elementary stream is passthrough, not re-encoded
//   4. the KLV PES timestamps are the ones we pushed (90 kHz rounding)
//   5. the source's own KLV / audio streams are dropped, not forwarded
//   6. a missing video source fails open_insert() and writes no output file
// argv: <video-source.ts> <input.klv> <temp.ts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "misbklv/backend.hpp"
#include "misbklv/gst_backend.hpp"
#include "misbklv/packet.hpp"

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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: gst_video_insert_test <video.ts> <input.klv> <out.ts>\n");
    return 2;
  }
  const std::string video_source = argv[1];
  const auto input = read_file(argv[2]);
  const std::string out_path = argv[3];
  std::span<const std::byte> buf = input;
  auto be = make_gst_backend();
  bool pass = true;
  auto check = [&](const char* what, bool ok) {
    std::printf("%-28s %s\n", what, ok ? "PASS" : "FAIL");
    pass = pass && ok;
  };

  // --- 6. missing source: clean failure, no output file ---------------------
  {
    const std::string missing_out = out_path + ".missing.ts";
    std::remove(missing_out.c_str());
    auto r = be->open_insert({"file:" + missing_out, false,
                              video_source + ".does-not-exist"});
    const bool errored = !r;
    std::ifstream f(missing_out, std::ios::binary);
    check("missing source rejected", errored && !f.good());
    if (f.good()) { f.close(); std::remove(missing_out.c_str()); }
  }

  // --- mux: video passthrough + KLV at known PTS ----------------------------
  // 100 ms apart, on the source's timeline from zero — the contract a video
  // branch imposes on the caller (kNoPts is rejected; checked below).
  constexpr std::int64_t kStepNs = 100'000'000;
  std::vector<std::int64_t> pushed;
  {
    auto ins = be->open_insert({"file:" + out_path, false, video_source});
    if (!ins) {
      std::fprintf(stderr, "open_insert failed: %d\n",
                   static_cast<int>(ins.error()));
      return 2;
    }
    check("kNoPts rejected",
          !(*ins)->push(buf.subspan(0, packet_frame_length(buf)), kNoPts));
    std::size_t off = 0;
    while (off < input.size()) {
      const std::size_t n = packet_frame_length(buf.subspan(off));
      if (n == 0) break;
      const std::int64_t pts = static_cast<std::int64_t>(pushed.size()) * kStepNs;
      if (!(*ins)->push(buf.subspan(off, n), pts)) {
        std::fprintf(stderr, "push failed at packet %zu\n", pushed.size());
        return 2;
      }
      pushed.push_back(pts);
      off += n;
    }
    if (!(*ins)->finish()) {
      std::fprintf(stderr, "finish failed\n");
      return 2;
    }
  }
  const auto out_ts = read_file(out_path.c_str());
  std::printf("muxed %zu KLV packets + video -> %s (%zu bytes)\n", pushed.size(),
              out_path.c_str(), out_ts.size());

  // --- 1. the PMT lists exactly one video ES and one KLV ES ------------------
  const auto es = read_pmt(out_ts);
  unsigned video_pid = 0, klv_pid = 0, nvideo = 0, nklv = 0;
  for (const auto& e : es) {
    std::printf("  ES pid=0x%04x stream_type=0x%02x%s\n", e.pid, e.stream_type,
                e.klva ? " KLVA" : "");
    if (is_video(e.stream_type)) { ++nvideo; if (!video_pid) video_pid = e.pid; }
    if (e.stream_type == 0x06 && e.klva) { ++nklv; klv_pid = e.pid; }
  }
  check("PMT: video + KLV, only", es.size() == 2 && nvideo == 1 && nklv == 1);

  // --- 2. KLV byte-exact through the mux ------------------------------------
  std::vector<std::byte> klv_back;
  auto r = be->extract(out_path, [&](const KlvPacket& kp) {
    klv_back.insert(klv_back.end(), kp.bytes.begin(), kp.bytes.end());
  });
  check("KLV byte-exact", r && klv_back == input);

  // --- 3. video is passthrough, not re-encoded ------------------------------
  const auto src_ts = read_file(video_source.c_str());
  const auto src_es = read_pmt(src_ts);
  unsigned src_video_pid = 0, src_video_type = 0;
  for (const auto& e : src_es)
    if (is_video(e.stream_type) && !src_video_pid) {
      src_video_pid = e.pid;
      src_video_type = e.stream_type;
    }
  const auto src_video = read_pes(src_ts, src_video_pid);
  const auto out_video = read_pes(out_ts, video_pid);
  std::printf("video ES: source %zu bytes / %zu PES, output %zu bytes / %zu PES\n",
              src_video.payload.size(), src_video.pts_90k.size(),
              out_video.payload.size(), out_video.pts_90k.size());
  bool same_type = false;
  for (const auto& e : es)
    if (e.pid == video_pid) same_type = (e.stream_type == src_video_type);
  check("video codec unchanged", same_type && src_video_type != 0);
  check("video ES byte-exact", out_video.payload == src_video.payload);
  check("video frame count kept",
        out_video.pts_90k.size() == src_video.pts_90k.size());

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
  std::printf("KLV PTS base %lld, video PTS base %lld (90 kHz)\n",
              static_cast<long long>(base), static_cast<long long>(vbase));
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

  std::printf("VIDEO PASSTHROUGH: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
