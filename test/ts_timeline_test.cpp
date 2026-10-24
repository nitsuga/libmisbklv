// SPDX-License-Identifier: Apache-2.0
// extract_ts_klv_timed provenance (issue #111) over hand-built synthetic TS:
// non-zero source origin, no-timestamp, 33-bit wrap, discontinuity_indicator,
// and a buffer that begins mid-stream. No fixtures, no gstreamer.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "misbklv/ts.hpp"

using namespace misbklv;

namespace {

int g_fail = 0;
bool check(bool ok, const char* what) {
  std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
  return ok;
}

constexpr std::int64_t kMod = TsTimeline::kPtsModulus;
constexpr std::uint16_t kKlvPid = 0x0101;
constexpr std::uint16_t kVideoPid = 0x0100;

std::byte B(int v) { return static_cast<std::byte>(v & 0xFF); }

// A minimal valid KLV packet: ST 0601 key, BER length, one tag-1 (checksum-less
// is fine for framing) TLV carrying `marker`.
std::vector<std::byte> klv(std::uint8_t marker) {
  std::vector<std::byte> p;
  for (int b : {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x0b, 0x01, 0x01, 0x0e, 0x01, 0x03, 0x01, 0x01, 0x00,
                0x00, 0x00})
    p.push_back(B(b));
  p.push_back(B(3));  // BER length
  p.push_back(B(0x41));
  p.push_back(B(1));
  p.push_back(B(marker));
  return p;
}

// PES with a 5-byte PTS field (or none if pts < 0), then payload.
std::vector<std::byte> pes(std::int64_t pts, const std::vector<std::byte>& payload,
                           int stream_id = 0xBD) {
  std::vector<std::byte> v = {B(0), B(0), B(1), B(stream_id)};
  const std::size_t hdr = pts >= 0 ? 8 : 3;
  const std::size_t len = hdr + payload.size();
  v.push_back(B(len >> 8));
  v.push_back(B(len));
  v.push_back(B(0x80));
  v.push_back(B(pts >= 0 ? 0x80 : 0x00));
  v.push_back(B(pts >= 0 ? 5 : 0));
  if (pts >= 0) {
    v.push_back(B(0x21 | ((pts >> 29) & 0x0e)));
    v.push_back(B(pts >> 22));
    v.push_back(B(0x01 | ((pts >> 14) & 0xfe)));
    v.push_back(B(pts >> 7));
    v.push_back(B(0x01 | ((pts << 1) & 0xfe)));
  }
  v.insert(v.end(), payload.begin(), payload.end());
  return v;
}

// Append `pes_bytes` to `ts` on `pid`, one TS packet per `chunk` payload bytes
// (default: all in one packet, padded via adaptation field). `disc` sets
// discontinuity_indicator on the first packet; `disc_on_continuation` sets it on
// the second packet (a non-PUSI one).
void add(std::vector<std::byte>& ts, std::uint16_t pid, const std::vector<std::byte>& pes_bytes,
         bool disc = false, std::size_t chunk = 183, bool disc_on_continuation = false) {
  for (std::size_t off = 0, n = 0; off < pes_bytes.size(); off += chunk, ++n) {
    const std::size_t take = std::min(chunk, pes_bytes.size() - off);
    std::array<std::byte, 188> pkt{};
    pkt[0] = B(0x47);
    pkt[1] = B((n == 0 ? 0x40 : 0) | (pid >> 8));
    pkt[2] = B(pid);
    pkt[3] = B(0x30);
    const std::size_t adapt = 183 - take;
    pkt[4] = B(adapt);
    if (adapt > 0) {
      const bool d = (n == 0 && disc) || (n == 1 && disc_on_continuation);
      pkt[5] = B(d ? 0x80 : 0x00);
      std::fill(pkt.begin() + 6, pkt.begin() + 5 + adapt, B(0xFF));
    }
    std::copy_n(pes_bytes.begin() + off, take, pkt.begin() + 5 + adapt);
    ts.insert(ts.end(), pkt.begin(), pkt.end());
  }
}

struct Got {
  std::int64_t pts_ns, src;
  std::int32_t epoch;
};

struct Run {
  std::vector<Got> pkts;
  TsTimeline tl;
  bool ok;
};

Run run(const std::vector<std::byte>& ts) {
  Run r;
  auto res = extract_ts_klv_timed(
      ts,
      [&](const TsKlvPacket& p) {
        r.pkts.push_back({p.packet.pts_ns, p.source_pts_90k, p.pts_epoch});
      },
      &r.tl);
  r.ok = static_cast<bool>(res);
  return r;
}

std::int64_t ns(std::int64_t ticks) { return ticks * 100'000 / 9; }

}  // namespace

// Unrolled source time of packet `g` relative to the unrolled origin, in ticks.
std::int64_t unrolled_offset(const Run& r, const Got& g) {
  return (g.src + g.epoch * kMod) - (r.tl.origin_pts_90k + r.tl.origin_pts_epoch * kMod);
}

int main() {
  using C = TsTimeline::Continuity;

  std::printf("non-zero origin, video earlier than KLV\n");
  {
    std::vector<std::byte> ts;
    add(ts, kVideoPid, pes(900'000, {B(0)}));
    add(ts, kKlvPid, pes(903'600, klv(1)));
    add(ts, kKlvPid, pes(907'200, klv(2)));
    auto r = run(ts);
    if (check(r.ok && r.pkts.size() == 2, "two packets")) {
      check(r.tl.continuity == C::Continuous, "continuous");
      check(r.tl.origin_pts_90k == 900'000, "origin is the earliest PTS across PIDs");
      check(r.pkts[0].src == 903'600 && r.pkts[0].pts_ns == ns(3'600), "packet 0 raw + normalized");
      check(r.pkts[1].src == 907'200 && r.pkts[1].pts_ns == ns(7'200), "packet 1 raw + normalized");
      check(!r.tl.pts_wrap_observed && !r.tl.discontinuity_indicator_observed &&
                !r.tl.pts_regression_observed,
            "no flags");
    }
  }

  std::printf("no timestamps anywhere\n");
  {
    std::vector<std::byte> ts;
    add(ts, kKlvPid, pes(-1, klv(1)));
    auto r = run(ts);
    if (check(r.ok && r.pkts.size() == 1, "one packet")) {
      check(r.tl.continuity == C::NoTimestamps && r.tl.origin_pts_90k == -1, "NoTimestamps");
      check(r.pkts[0].pts_ns == kNoPts && r.pkts[0].src == -1, "kNoPts / source -1");
    }
  }

  std::printf("timestamps present but this PES untimed\n");
  {
    std::vector<std::byte> ts;
    add(ts, kVideoPid, pes(1000, {B(0)}));
    add(ts, kKlvPid, pes(-1, klv(1)));
    auto r = run(ts);
    check(r.tl.continuity == C::Continuous, "stream is timed");
    if (check(r.pkts.size() == 1, "one packet"))
      check(r.pkts[0].pts_ns == kNoPts && r.pkts[0].src == -1,
            "untimed PES stays kNoPts, distinct from NoTimestamps");
  }

  std::printf("PES header split across TS packets (scan and extraction must agree)\n");
  {
    std::vector<std::byte> ts;
    // 8 bytes per TS packet: the 14-byte header needs two packets.
    add(ts, kVideoPid, pes(1001, {B(0)}));
    add(ts, kKlvPid, pes(1000, klv(1)), false, 8);
    auto r = run(ts);
    if (check(r.ok && r.pkts.size() == 1, "one packet")) {
      check(r.tl.origin_pts_90k == 1000, "origin sees the split-header PTS");
      check(r.pkts[0].src == 1000 && r.pkts[0].pts_ns == 0, "non-negative pts_ns, exact source");
    }
    std::vector<std::byte> alone;
    add(alone, kKlvPid, pes(1000, klv(1)), false, 8);
    auto a = run(alone);
    check(a.tl.continuity == C::Continuous && a.tl.origin_pts_90k == 1000 &&
              a.pkts.size() == 1 && a.pkts[0].src == 1000,
          "split-header PES alone is not NoTimestamps");
  }

  std::printf("33-bit wrap\n");
  {
    std::vector<std::byte> ts;
    add(ts, kKlvPid, pes(kMod - 7'200, klv(1)));
    add(ts, kKlvPid, pes(kMod - 3'600, klv(2)));
    add(ts, kKlvPid, pes(0, klv(3)));
    add(ts, kKlvPid, pes(3'600, klv(4)));
    auto r = run(ts);
    std::vector<std::int64_t> legacy;
    extract_ts_klv(ts, [&](const KlvPacket& p) { legacy.push_back(p.pts_ns); });
    if (check(r.ok && r.pkts.size() == 4 && legacy.size() == 4, "four packets")) {
      check(r.tl.pts_wrap_observed && r.tl.continuity == C::Interrupted, "wrap -> Interrupted");
      check(r.tl.origin_pts_90k == 0 && r.tl.origin_pts_epoch == 1,
            "origin is the raw minimum, in epoch 1");
      check(r.pkts[0].src == kMod - 7'200 && r.pkts[3].src == 3'600, "raw PTS preserved");
      check(r.pkts[0].epoch == 0 && r.pkts[1].epoch == 0 && r.pkts[2].epoch == 1 &&
                r.pkts[3].epoch == 1,
            "epoch increments at the wrap");
      bool same = true;
      for (int i = 0; i < 4; ++i) same = same && legacy[i] == r.pkts[i].pts_ns;
      check(same, "pts_ns identical to the legacy overload across the wrap");
      check(unrolled_offset(r, r.pkts[0]) == -7'200 && unrolled_offset(r, r.pkts[3]) == 3'600,
            "unrolled offsets from the unrolled origin are exact across the wrap");
    }
  }

  std::printf("KLV lines up with video that wrapped first (all-PID epoch)\n");
  {
    std::vector<std::byte> ts;
    add(ts, kVideoPid, pes(kMod - 3'600, {B(0)}));
    add(ts, kVideoPid, pes(0, {B(0)}));
    add(ts, kKlvPid, pes(1'800, klv(1)));
    auto r = run(ts);
    if (check(r.pkts.size() == 1, "one packet")) {
      check(r.tl.pts_wrap_observed && r.pkts[0].epoch == 1, "KLV epoch counts the video wrap");
      check(unrolled_offset(r, r.pkts[0]) == 1'800, "KLV lands 1800 ticks after the origin");
    }
  }

  std::printf("reordered PES near the wrap\n");
  {
    std::vector<std::byte> ts;
    add(ts, kVideoPid, pes(kMod - 100, {B(0)}));
    add(ts, kVideoPid, pes(50, {B(0)}));
    add(ts, kVideoPid, pes(kMod - 50, {B(0)}));  // B-frame from before the wrap
    add(ts, kKlvPid, pes(3'600, klv(1)));
    auto r = run(ts);
    check(r.tl.pts_wrap_observed, "wrap seen");
    if (check(r.pkts.size() == 1, "one packet"))
      check(r.pkts[0].epoch == 1 && unrolled_offset(r, r.pkts[0]) == 3'550,
            "KLV after the wrap is in epoch 1");
  }

  std::printf("buffer starting after a wrap but holding a pre-wrap frame is not Continuous\n");
  {
    std::vector<std::byte> ts;
    add(ts, kKlvPid, pes(1'000, klv(1)));
    add(ts, kVideoPid, pes(kMod - 3'600, {B(0)}));  // reordered from before the wrap
    add(ts, kKlvPid, pes(100, klv(2)));
    auto r = run(ts);
    check(r.tl.pts_wrap_observed && r.tl.continuity == C::Interrupted, "Interrupted");
  }

  std::printf("large unflagged backward step\n");
  {
    std::vector<std::byte> ts;
    add(ts, kKlvPid, pes(2'000'000'000, klv(1)));
    add(ts, kKlvPid, pes(1'000'000'000, klv(2)));
    auto r = run(ts);
    check(r.tl.pts_regression_observed && !r.tl.pts_wrap_observed &&
              r.tl.continuity == C::Interrupted,
          "regression -> Interrupted, not a wrap");
    std::vector<std::byte> ok;
    add(ok, kKlvPid, pes(100'000, klv(1)));
    add(ok, kKlvPid, pes(100'000 - 9'000, klv(2)));  // 100 ms reorder
    check(run(ok).tl.continuity == C::Continuous, "small reorder stays Continuous");
  }

  std::printf("discontinuity_indicator\n");
  {
    std::vector<std::byte> ts;
    add(ts, kKlvPid, pes(1000, klv(1)));
    add(ts, kKlvPid, pes(2000, klv(2)), /*disc=*/true);
    auto r = run(ts);
    check(r.tl.discontinuity_indicator_observed && r.tl.continuity == C::Interrupted,
          "indicator -> Interrupted");
    check(!r.tl.pts_wrap_observed, "not a wrap");
    std::vector<std::byte> cont;  // indicator on a non-PUSI (continuation) packet
    add(cont, kKlvPid, pes(1000, klv(1)), false, 8, /*disc_on_continuation=*/true);
    check(run(cont).tl.discontinuity_indicator_observed, "indicator on a continuation packet");
  }

  std::printf("0xFC (RP 217) cells carry the same provenance\n");
  {
    std::vector<std::byte> cell = {B(0), B(0), B(0), B(0), B(0)};
    const auto k = klv(7);
    cell[3] = B(k.size() >> 8);
    cell[4] = B(k.size());
    cell.insert(cell.end(), k.begin(), k.end());
    std::vector<std::byte> ts;
    add(ts, kVideoPid, pes(500, {B(0)}));
    add(ts, kKlvPid, pes(4'100, cell, 0xFC));
    auto r = run(ts);
    if (check(r.ok && r.pkts.size() == 1, "one packet")) {
      check(r.pkts[0].src == 4'100 && r.pkts[0].pts_ns == ns(3'600), "raw + normalized");
    }
  }

  std::printf("timeline is filled on error\n");
  {
    std::vector<std::byte> ts;
    add(ts, kVideoPid, pes(900, {B(0)}));
    auto bad = klv(1);
    bad.resize(10);  // truncated frame
    add(ts, kKlvPid, pes(1000, bad));
    auto r = run(ts);
    check(!r.ok, "extraction fails");
    check(r.tl.continuity == C::Continuous && r.tl.origin_pts_90k == 900,
          "timeline still describes the buffer");
  }

  std::printf("buffer beginning mid-stream\n");
  {
    std::vector<std::byte> whole, tail;
    for (int i = 0; i < 4; ++i) add(whole, kKlvPid, pes(100'000 + i * 3'600, klv(i)));
    tail.assign(whole.begin() + 2 * 188, whole.end());
    auto w = run(whole);
    auto t = run(tail);
    if (check(w.pkts.size() == 4 && t.pkts.size() == 2, "packet counts")) {
      check(w.tl.origin_pts_90k == 100'000, "whole stream anchors at its start");
      check(t.tl.origin_pts_90k == 107'200 && t.pkts[0].pts_ns == 0,
            "tail re-anchors, and origin says which source tick that was");
      check(t.pkts[0].src == w.pkts[2].src, "raw PTS identifies the packet in both");
      check(w.pkts[2].pts_ns - t.pkts[0].pts_ns == ns(7'200), "consumer can reconcile via origin");
    }
  }

  std::printf("legacy overload unchanged\n");
  {
    std::vector<std::byte> ts;
    add(ts, kKlvPid, pes(5'000, klv(1)));
    add(ts, kKlvPid, pes(8'600, klv(2)));
    std::vector<std::int64_t> v;
    auto res = extract_ts_klv(ts, [&](const KlvPacket& p) { v.push_back(p.pts_ns); });
    check(res && v.size() == 2 && v[0] == 0 && v[1] == ns(3'600), "pts_ns as before");
  }

  std::printf("%s\n", g_fail ? "TS TIMELINE FAIL" : "TS TIMELINE PASS");
  return g_fail ? 1 : 0;
}
