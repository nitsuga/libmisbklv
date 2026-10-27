// SPDX-License-Identifier: Apache-2.0
// TsKlvStream must match extract_ts_klv_timed on every input, however the bytes are
// chunked: same packets (bytes, raw PTS, wrap epoch), same TsTimeline, same result.
// `pts_ns` is the one documented difference (kNoPts when streaming). Hand-built cases
// cover the pre-pass edge cases; a seeded generator covers header-spanning PES,
// incomplete headers, competing KLV candidates, wraps, regressions, discontinuities,
// garbage and truncation. No fixtures, no gstreamer.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "misbklv/ts.hpp"

using namespace misbklv;

namespace {

int g_fail = 0;
bool check(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("  FAIL: %s\n", what.c_str());
    ++g_fail;
  }
  return ok;
}

std::byte B(int v) { return static_cast<std::byte>(v & 0xFF); }

std::vector<std::byte> klv(std::uint8_t marker, bool bad_length = false) {
  std::vector<std::byte> p;
  for (int b : {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x0b, 0x01, 0x01, 0x0e, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0x00})
    p.push_back(B(b));
  if (bad_length) {
    p.push_back(B(0x84));  // BER long form claiming a huge length
    for (int b : {0xff, 0xff, 0xff, 0xff}) p.push_back(B(b));
    return p;
  }
  p.push_back(B(3));
  p.push_back(B(0x41));
  p.push_back(B(1));
  p.push_back(B(marker));
  return p;
}

std::vector<std::byte> pes(std::int64_t pts, const std::vector<std::byte>& payload) {
  std::vector<std::byte> v = {B(0), B(0), B(1), B(0xBD)};
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

// One TS packet per `chunk` PES bytes (padded with adaptation stuffing), so a small
// `chunk` makes the 14 PES header bytes span several packets.
void add(std::vector<std::byte>& ts, std::uint16_t pid, const std::vector<std::byte>& pes_bytes, bool disc = false,
         std::size_t chunk = 183) {
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
      pkt[5] = B(n == 0 && disc ? 0x80 : 0x00);
      std::fill(pkt.begin() + 6, pkt.begin() + 5 + adapt, B(0xFF));
    }
    std::copy_n(pes_bytes.begin() + off, take, pkt.begin() + 5 + adapt);
    ts.insert(ts.end(), pkt.begin(), pkt.end());
  }
}

struct Out {
  struct P {
    std::vector<std::byte> bytes;
    std::int64_t src;
    std::int32_t epoch;
    bool operator==(const P&) const = default;
  };
  std::vector<P> packets;
  TsTimeline tl;
  int status = 0;  // 0 ok, else 1 + Error
};

int code(const Result<std::monostate>& r) { return r ? 0 : 1 + static_cast<int>(r.error()); }

Out batch(const std::vector<std::byte>& ts) {
  Out o;
  const auto r = extract_ts_klv_timed(ts, [&](const TsKlvPacket& p) {
    o.packets.push_back({std::vector<std::byte>(p.packet.bytes.begin(), p.packet.bytes.end()), p.source_pts_90k, p.pts_epoch});
  }, &o.tl);
  o.status = code(r);
  return o;
}

Out stream(const std::vector<std::byte>& ts, std::size_t chunk, std::mt19937* jitter = nullptr) {
  Out o;
  TsKlvStream s([&](const TsKlvPacket& p) {
    check(p.packet.pts_ns == kNoPts, "streaming pts_ns is kNoPts");
    o.packets.push_back({std::vector<std::byte>(p.packet.bytes.begin(), p.packet.bytes.end()), p.source_pts_90k, p.pts_epoch});
  });
  for (std::size_t off = 0; off < ts.size();) {
    std::size_t n = chunk ? chunk : ts.size();
    if (jitter) n = 1 + (*jitter)() % 700;
    n = std::min(n, ts.size() - off);
    (void)s.feed(std::span<const std::byte>(ts).subspan(off, n));
    off += n;
  }
  o.status = code(s.finish(&o.tl));
  return o;
}

bool same_timeline(const TsTimeline& a, const TsTimeline& b) {
  return a.continuity == b.continuity && a.origin_pts_90k == b.origin_pts_90k && a.origin_pts_epoch == b.origin_pts_epoch &&
         a.pts_wrap_observed == b.pts_wrap_observed && a.pts_regression_observed == b.pts_regression_observed &&
         a.discontinuity_indicator_observed == b.discontinuity_indicator_observed;
}

void compare(const std::vector<std::byte>& ts, const std::string& name) {
  const Out want = batch(ts);
  std::mt19937 jitter(7);
  for (std::size_t chunk : {std::size_t{0}, std::size_t{1}, std::size_t{187}, std::size_t{188}, std::size_t{189},
                            std::size_t{1000}, std::size_t{4096}, std::size_t{65537}, std::size_t{999999}}) {
    const Out got = stream(ts, chunk);
    const std::string tag = name + " chunk=" + std::to_string(chunk);
    check(got.status == want.status, tag + ": status " + std::to_string(got.status) + " vs " + std::to_string(want.status));
    check(got.packets == want.packets, tag + ": packets (" + std::to_string(got.packets.size()) + " vs " +
                                           std::to_string(want.packets.size()) + ")");
    check(same_timeline(got.tl, want.tl), tag + ": timeline");
  }
  const Out got = stream(ts, 0, &jitter);
  check(got.status == want.status && got.packets == want.packets && same_timeline(got.tl, want.tl), name + ": jittered chunks");
}

constexpr std::int64_t kMod = TsTimeline::kPtsModulus;

// --- seeded generator -------------------------------------------------------
std::vector<std::byte> random_stream(std::uint32_t seed) {
  std::mt19937 rng(seed);
  auto pick = [&](int n) { return static_cast<int>(rng() % n); };
  std::vector<std::byte> ts;
  std::int64_t clock = pick(4) == 0 ? kMod - 200000 : 900000 + pick(100000);
  const int count = 8 + pick(40);
  for (int i = 0; i < count; ++i) {
    const std::uint16_t pid = std::array<std::uint16_t, 4>{0x100, 0x101, 0x101, 0x102}[pick(4)];
    std::int64_t pts = -1;
    if (pick(8) != 0) {
      clock += 3000 * (1 + pick(3));
      if (pick(20) == 0) clock -= 4 * 90000 * (1 + pick(1000) / 100);  // regression (sometimes past tolerance)
      if (pick(25) == 0) clock += kMod / 2 + 5;                          // forward half-range jump
      if (clock >= kMod) clock -= kMod;                                  // wrap
      if (clock < 0) clock += kMod;
      pts = clock;
    }
    std::vector<std::byte> payload;
    if (pid == 0x101 || (pid == 0x102 && pick(6) == 0)) payload = klv(static_cast<std::uint8_t>(i), pick(40) == 0);
    else payload.assign(1 + pick(300), B(0x55));
    const std::size_t chunk = pick(3) == 0 ? 1 + pick(20) : 183;  // tiny chunks: header spans packets
    add(ts, pid, pes(pts, payload), pick(30) == 0, chunk);
    if (pick(25) == 0) ts.insert(ts.end(), 188, B(0x12));  // garbage slot, bad sync
    if (pick(15) == 0) {                                   // a PES too short to hold its 14 header bytes
      std::vector<std::byte> stub = {B(0), B(0), B(1), B(0xBD), B(0), B(3), B(0x80), B(0x80), B(0)};
      add(ts, static_cast<std::uint16_t>(0x103 + pick(3)), stub);
    }
  }
  if (pick(5) == 0) ts.resize(ts.size() - pick(300));  // truncated tail
  return ts;
}

}  // namespace

int main() {
  {
    std::vector<std::byte> ts;
    add(ts, 0x100, pes(900'000, {B(0)}));
    add(ts, 0x101, pes(903'600, klv(1)));
    add(ts, 0x101, pes(907'200, klv(2)));
    compare(ts, "basic");
  }
  {
    std::vector<std::byte> ts;
    add(ts, 0x100, pes(kMod - 6000, {B(0)}));
    add(ts, 0x101, pes(kMod - 3000, klv(1)));
    add(ts, 0x101, pes(2000, klv(2)));
    add(ts, 0x100, pes(kMod - 1000, {B(0)}));  // reordered from before the wrap
    compare(ts, "wrap");
  }
  {
    std::vector<std::byte> ts;
    add(ts, 0x101, pes(-1, klv(1)));
    compare(ts, "no timestamps");
  }
  {
    std::vector<std::byte> ts;
    add(ts, 0x100, pes(900'000, {B(0)}), true);
    add(ts, 0x101, pes(903'600, klv(1)));
    compare(ts, "discontinuity indicator");
  }
  {
    std::vector<std::byte> ts;
    add(ts, 0x101, pes(903'600, klv(1)), false, 5);  // header spans four packets
    add(ts, 0x100, pes(1, {B(0)}), false, 3);
    compare(ts, "header across packets");
  }
  {
    std::vector<std::byte> ts;
    add(ts, 0x101, pes(903'600, klv(1, true)));
    compare(ts, "bad BER length");
  }
  {
    std::vector<std::byte> ts;
    add(ts, 0x100, pes(900'000, {B(0)}));
    ts.insert(ts.end(), 100, B(0));
    compare(ts, "no KLV, trailing partial packet");
  }
  for (const char* fixture : {"synthetic-timed-0x06.ts", "synthetic-timed-0x15.ts", "synthetic-untimed-0x06.ts", "synthetic-video.ts"}) {
    std::ifstream in(std::string(MISBKLV_FIXTURE_DIR) + "/" + fixture, std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
    if (raw.empty()) { check(false, std::string("missing fixture ") + fixture); continue; }
    std::vector<std::byte> ts(raw.size());
    std::transform(raw.begin(), raw.end(), ts.begin(), [](char c) { return static_cast<std::byte>(c); });
    compare(ts, fixture);
  }
  // One damaged start (a PUSI packet with no payload) must not stall later delivery or fail the stream.
  {
    std::vector<std::byte> ts;
    std::array<std::byte, 188> stray{};
    stray.fill(B(0xFF));
    stray[0] = B(0x47); stray[1] = B(0x42); stray[2] = B(0x00); stray[3] = B(0x30); stray[4] = B(183); stray[5] = B(0);
    ts.insert(ts.end(), stray.begin(), stray.end());
    for (int i = 0; i < 70000; ++i) add(ts, 0x101, pes(900'000 + 3000 * i, klv(static_cast<std::uint8_t>(i))));
    const Out want = batch(ts);
    std::size_t delivered = 0;
    TsKlvStream::Stats worst{};
    TsKlvStream s([&](const TsKlvPacket&) { ++delivered; });
    for (std::size_t off = 0; off < ts.size(); off += 4096) {
      (void)s.feed(std::span<const std::byte>(ts).subspan(off, std::min<std::size_t>(4096, ts.size() - off)));
      const auto st = s.stats();
      worst.unresolved_starts = std::max(worst.unresolved_starts, st.unresolved_starts);
      worst.deferred_jobs = std::max(worst.deferred_jobs, st.deferred_jobs);
    }
    check(delivered > 69000, "damaged start: delivery continues before finish (" + std::to_string(delivered) + ")");
    check(worst.unresolved_starts <= 16500 && worst.deferred_jobs <= 16500, "damaged start: bookkeeping stays within the header window");
    TsTimeline tl;
    check(code(s.finish(&tl)) == want.status && want.status == 0, "damaged start: same result as the span overload");
    check(delivered == want.packets.size() && same_timeline(tl, want.tl), "damaged start: same packets and timeline");
  }
  // Wrap epochs are retained only for PES that may still be framed, however long the file runs after the wrap.
  {
    std::vector<std::byte> ts;
    std::int64_t pts = kMod - 30000;
    for (int i = 0; i < 20000; ++i) {
      for (int v = 0; v < 10; ++v) { add(ts, 0x100, pes(pts % kMod, {B(0)})); pts += 300; }
      add(ts, 0x101, pes(pts % kMod, klv(static_cast<std::uint8_t>(i))));
    }
    TsKlvStream::Stats worst{};
    TsKlvStream s([&](const TsKlvPacket&) {});
    for (std::size_t off = 0; off < ts.size(); off += 65536) {
      (void)s.feed(std::span<const std::byte>(ts).subspan(off, std::min<std::size_t>(65536, ts.size() - off)));
      worst.epoch_entries = std::max(worst.epoch_entries, s.stats().epoch_entries);
    }
    TsTimeline tl;
    check(code(s.finish(&tl)) == 0 && tl.pts_wrap_observed, "wrap run: extracted and wrap seen");
    check(worst.epoch_entries <= 64, "wrap run: epoch entries bounded (" + std::to_string(worst.epoch_entries) + ")");
    compare(ts, "wrap run");
  }
  // The one documented difference: a header whose remaining bytes arrive more than 16384 packets later is untimed here.
  {
    std::vector<std::byte> late;
    add(late, 0x103, pes(1000, {B(0)}), false, 5);  // header spans several packets
    std::vector<std::byte> ts(late.begin(), late.begin() + 188);
    for (int i = 0; i < 17000; ++i) add(ts, 0x100, pes(900'000 + 3000 * i, {B(0)}));
    ts.insert(ts.end(), late.begin() + 188, late.end());
    add(ts, 0x101, pes(950'000, klv(1)));
    const Out want = batch(ts);
    const Out got = stream(ts, 4096);
    check(want.tl.origin_pts_90k == 1000, "late header: span overload sees the late PTS");
    check(got.tl.origin_pts_90k == 900'000, "late header: stream treats the start as untimed");
    check(got.status == want.status && got.packets == want.packets, "late header: packets and result still match");
  }
  // The window can also change later epochs (a late PTS is never shown to the unroller) and the wrap flag; and a
  // KLV PES whose own header is late is framed untimed rather than with a PTS and a guessed epoch.
  {
    std::vector<std::byte> late;
    add(late, 0x103, pes(kMod - 1000, {B(0)}), false, 5);
    std::vector<std::byte> ts(late.begin(), late.begin() + 188);
    for (int i = 0; i < 17000; ++i) {
      std::array<std::byte, 188> filler{};
      filler.fill(B(0xFF));
      filler[0] = B(0x47); filler[1] = B(0x01); filler[2] = B(0x00); filler[3] = B(0x10);
      ts.insert(ts.end(), filler.begin(), filler.end());
    }
    ts.insert(ts.end(), late.begin() + 188, late.end());
    for (int i = 0; i < 3; ++i) add(ts, 0x101, pes(2000 + 3000 * i, klv(static_cast<std::uint8_t>(i))));
    const Out want = batch(ts);
    const Out got = stream(ts, 4096);
    check(want.tl.pts_wrap_observed && !got.tl.pts_wrap_observed, "late non-KLV start: the wrap goes unseen");
    check(want.packets.size() == 3 && got.packets.size() == 3 && want.packets[0].epoch == 1 && got.packets[0].epoch == 0,
          "late non-KLV start: later epochs differ");
  }
  {
    std::vector<std::byte> late;
    add(late, 0x101, pes(4000, klv(9)), false, 5);
    std::vector<std::byte> ts;
    add(ts, 0x101, pes(kMod - 3000, klv(1)));
    add(ts, 0x100, pes(1000, {B(0)}));  // the wrap
    ts.insert(ts.end(), late.begin(), late.begin() + 188);
    for (int i = 0; i < 17000; ++i) {
      std::array<std::byte, 188> filler{};
      filler.fill(B(0xFF));
      filler[0] = B(0x47); filler[1] = B(0x01); filler[2] = B(0x02); filler[3] = B(0x10);
      ts.insert(ts.end(), filler.begin(), filler.end());
    }
    ts.insert(ts.end(), late.begin() + 188, late.end());
    const Out want = batch(ts);
    const Out got = stream(ts, 4096);
    check(want.packets.size() == 2 && got.packets.size() == 2, "late KLV header: both packets delivered");
    if (want.packets.size() == 2 && got.packets.size() == 2) {
      check(want.packets[1].src == 4000 && want.packets[1].epoch == 1, "late KLV header: span overload times it with the wrap epoch");
      check(got.packets[1].src == -1 && got.packets[1].epoch == 0, "late KLV header: stream frames it untimed, never a PTS with a wrong epoch");
    }
  }
  // A timed start that extraction can never frame (no payload bytes in its first packet) holds no state.
  {
    std::vector<std::byte> ts;
    add(ts, 0x101, pes(kMod - 3000, klv(0)));
    for (int i = 0; i < 20000; ++i) {
      add(ts, 0x101, pes((2000 + 3000 * i) % kMod, klv(static_cast<std::uint8_t>(i))));
      std::array<std::byte, 188> stray{};
      stray.fill(B(0xFF));
      stray[0] = B(0x47); stray[1] = B(0x41); stray[2] = B(0x01); stray[3] = B(0x20); stray[4] = B(183); stray[5] = B(0);
      ts.insert(ts.end(), stray.begin(), stray.end());
      std::array<std::byte, 188> rest{};  // continuation carrying a timed PES header
      rest.fill(B(0xFF));
      rest[0] = B(0x47); rest[1] = B(0x01); rest[2] = B(0x01); rest[3] = B(0x30); rest[4] = B(183 - 14); rest[5] = B(0);
      const auto header = pes(5000 + 3000 * i, {});
      std::copy_n(header.begin(), 14, rest.begin() + 5 + 183 - 14);
      ts.insert(ts.end(), rest.begin(), rest.end());
    }
    TsKlvStream s([&](const TsKlvPacket&) {});
    std::size_t worst = 0;
    for (std::size_t off = 0; off < ts.size(); off += 65536) {
      (void)s.feed(std::span<const std::byte>(ts).subspan(off, std::min<std::size_t>(65536, ts.size() - off)));
      worst = std::max(worst, s.stats().epoch_entries);
    }
    check(worst <= 64, "unframeable starts: epoch entries bounded (" + std::to_string(worst) + ")");
    compare(ts, "unframeable starts");
  }
  for (std::uint32_t seed = 1; seed <= 600; ++seed) compare(random_stream(seed), "seed " + std::to_string(seed));
  std::printf("%s\n", g_fail ? "ts_stream_test FAILED" : "ts_stream_test passed");
  return g_fail ? 1 : 0;
}
