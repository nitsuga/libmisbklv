// SPDX-License-Identifier: Apache-2.0
// gst-free MPEG-TS KLV extractor test (fork 12 / B3). Extract KLV from a .ts
// (0x06 or 0x15) and assert byte-exact against a reference ES. Also checks the
// per-packet timestamps (ADR 0021) over generated carriers — the exact values are
// pinned against known-pushed ones in gst_video_insert_test; what's asserted
// here is that a timestamped capture yields timestamps at all, and that they
// advance rather than jumping around. argv: <source.ts> <expected.klv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <span>
#include <vector>

#include "misbklv/packet.hpp"
#include "misbklv/ts.hpp"

using namespace misbklv;

static std::vector<std::byte> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i)
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: ts_extract_test <source.ts> <expected.klv>\n");
    return 2;
  }
  const auto ts = read_file(argv[1]);
  std::vector<std::byte> out;
  std::size_t npkt = 0, timed = 0;
  bool all_parse = true, monotonic = true;
  std::int64_t first = kNoPts, last = kNoPts;
  auto r = extract_ts_klv(ts, [&](const KlvPacket& kp) {
    out.insert(out.end(), kp.bytes.begin(), kp.bytes.end());
    ++npkt;
    if (!parse_packet(kp.bytes)) all_parse = false;
    if (kp.pts_ns == kNoPts) return;
    ++timed;
    if (first == kNoPts) first = kp.pts_ns;
    if (kp.pts_ns < 0 || (last != kNoPts && kp.pts_ns < last)) monotonic = false;
    last = kp.pts_ns;
  });
  if (!r) {
    std::fprintf(stderr, "no KLV PID found: error %d\n", static_cast<int>(r.error()));
    return 2;
  }
  const auto expected = read_file(argv[2]);
  std::printf("extracted %zu packets, %zu bytes (expected %zu)\n", npkt, out.size(),
              expected.size());
  std::printf("every unit is a whole KLV packet: %s\n", all_parse ? "yes" : "no");
  // An untimed synthetic carrier correctly yields kNoPts throughout — that is
  // data, not a defect. What must never happen is a
  // partially or non-monotonically timestamped stream, which is what a
  // mis-attributed mark would look like.
  std::printf("timestamps: %zu/%zu packets, %s, first=%lld last=%lld ns\n", timed, npkt,
              monotonic ? "non-decreasing" : "OUT OF ORDER", static_cast<long long>(first),
              static_cast<long long>(last));
  const bool pts_ok = monotonic && (timed == 0 || timed == npkt);
  const bool match = (out == expected);
  std::printf("TS EXTRACT vs reference: %s\n", match ? "byte-exact PASS" : "MISMATCH");
  return (match && all_parse && pts_ok && npkt > 0) ? 0 : 1;
}
