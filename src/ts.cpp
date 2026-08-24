// SPDX-License-Identifier: Apache-2.0
#include "misbklv/ts.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "misbklv/backend.hpp"  // kDefaultMaxKlvPacketBytes
#include "misbklv/packet.hpp"   // inspect_packet_frame
#include "pts_marks.hpp"

namespace misbklv {
namespace {

constexpr std::uint8_t kSmpteUl[4] = {0x06, 0x0e, 0x2b, 0x34};
constexpr std::array<std::byte, 4> kSmpteUlPrefix = {
    std::byte{0x06}, std::byte{0x0e}, std::byte{0x2b}, std::byte{0x34}};
constexpr std::size_t kPkt = 188;

std::uint8_t u8(std::span<const std::byte> s, std::size_t i) {
  return std::to_integer<std::uint8_t>(s[i]);
}

bool starts_with_ul(std::span<const std::byte> s) {
  return s.size() >= 4 && u8(s, 0) == kSmpteUl[0] && u8(s, 1) == kSmpteUl[1] &&
         u8(s, 2) == kSmpteUl[2] && u8(s, 3) == kSmpteUl[3];
}

// Unwrap a full PES packet into its KLV payload (empty if not KLV/parseable).
std::span<const std::byte> unwrap_pes(std::span<const std::byte> pes) {
  if (pes.size() < 9 || u8(pes, 0) != 0 || u8(pes, 1) != 0 || u8(pes, 2) != 1)
    return {};
  const std::uint8_t stream_id = u8(pes, 3);
  const std::size_t pes_len = (u8(pes, 4) << 8) | u8(pes, 5);
  const std::size_t poff = ((u8(pes, 6) & 0xC0) == 0x80) ? 9 + u8(pes, 8) : 6;
  const std::size_t end =
      (pes_len > 0) ? std::min<std::size_t>(6 + pes_len, pes.size()) : pes.size();
  if (poff > end) return {};
  auto payload = pes.subspan(poff, end - poff);
  if (stream_id == 0xFC) {  // metadata AU cell (SMPTE RP 217): 5-byte header
    if (payload.size() < 5) return {};
    const std::size_t clen = (u8(payload, 3) << 8) | u8(payload, 4);
    return payload.subspan(5, std::min(clen, payload.size() - 5));
  }
  return payload;  // 0x06: KLV directly in the PES
}

// The PES header's 33-bit PTS in 90 kHz ticks, or -1 if the packet carries none
// (`PTS_DTS_flags` clear, or no optional header at all — a `stream_id` like
// private_stream_2 has none). Layout: ISO 13818-1 Table 2-21, marker bits and all.
std::int64_t pes_pts_90k(std::span<const std::byte> pes) {
  if (pes.size() < 14) return -1;
  if ((u8(pes, 6) & 0xC0) != 0x80) return -1;  // no optional PES header
  if ((u8(pes, 7) & 0x80) == 0) return -1;     // PTS_DTS_flags: no PTS
  return (static_cast<std::int64_t>(u8(pes, 9) & 0x0e) << 29) |
         (static_cast<std::int64_t>(u8(pes, 10)) << 22) |
         (static_cast<std::int64_t>(u8(pes, 11) & 0xfe) << 14) |
         (static_cast<std::int64_t>(u8(pes, 12)) << 7) |
         (static_cast<std::int64_t>(u8(pes, 13)) >> 1);
}

// The earliest PTS anywhere in the buffer, in 90 kHz ticks (-1 if nothing is
// timestamped). This is the origin `pts_ns` is measured from: the start of the
// source's presentation, which is what the insert path's timeline is also
// anchored to (ADR 0021). A cheap header-only pre-pass — it looks at the first
// 14 bytes of each PES, never at payload.
//
// Taking the minimum rather than the first in file order matters: with
// reordered video the first PES in the file is not the earliest presentation
// time, and picking it would shift every reported timestamp by the reorder
// delay.
std::int64_t earliest_pts_90k(std::span<const std::byte> ts) {
  std::int64_t best = -1;
  for (std::size_t i = 0; i + kPkt <= ts.size(); i += kPkt) {
    if (u8(ts, i) != 0x47) continue;
    if (!((u8(ts, i + 1) >> 6) & 1)) continue;  // not a PES start
    const std::uint8_t afc = (u8(ts, i + 3) >> 4) & 3;
    std::size_t off = i + 4;
    if (afc & 2) off += 1 + u8(ts, off);
    if (!(afc & 1) || off + 14 > i + kPkt) continue;
    auto pes = ts.subspan(off, (i + kPkt) - off);
    if (u8(pes, 0) != 0 || u8(pes, 1) != 0 || u8(pes, 2) != 1) continue;
    const std::int64_t pts = pes_pts_90k(pes);
    if (pts >= 0 && (best < 0 || pts < best)) best = pts;
  }
  return best;
}

}  // namespace

Result<std::monostate> extract_ts_klv(std::span<const std::byte> ts,
                                      const PacketHandler& on_packet) {
  std::unordered_map<std::uint16_t, std::vector<std::byte>> pes;  // PID -> current PES
  int klv_pid = -1;
  std::vector<std::byte> reasm;  // reassembled KLV for the KLV PID
  const std::int64_t origin_90k = earliest_pts_90k(ts);
  detail::PtsMarks marks;   // PES timestamps -> KLV packet timestamps
  std::size_t stream_off = 0;  // absolute offset of reasm[0] in the KLV stream
  std::optional<Error> frame_error;  // first framing failure, returned at the end

  auto flush = [&](std::uint16_t pid) {
    auto it = pes.find(pid);
    if (it == pes.end() || it->second.empty()) return;
    const auto klv = unwrap_pes(it->second);
    if (starts_with_ul(klv) && klv_pid < 0) klv_pid = pid;
    // `klv_pid >= 0` first: before any PID is selected, uint16_t(-1) would
    // alias the reserved PID 0xFFFF and let a payload-bearing packet pollute
    // the reassembly buffer ahead of PID selection.
    if (klv_pid >= 0 && pid == static_cast<std::uint16_t>(klv_pid) && !frame_error) {
      // ns from the start of the source's presentation; kNoPts when this PES
      // (or the whole file) carries no PTS. 90 kHz -> ns is exact at 1e5/9.
      //
      // A KLV packet may span several PES packets (the 16-bit
      // PES_packet_length ceiling, or a muxer splitting a large packet), so
      // every KLV-PID payload is appended here regardless of whether it starts
      // with the UL — the UL prefix only selects the PID, once, at the first
      // payload that carries it.
      const std::int64_t pts90 = pes_pts_90k(it->second);
      marks.mark(stream_off + reasm.size(),
                 (pts90 < 0 || origin_90k < 0)
                     ? kNoPts
                     : (pts90 - origin_90k) * 100'000 / 9);
      reasm.insert(reasm.end(), klv.begin(), klv.end());
      // Frame as much of `reasm` as is complete, resyncing over garbage the
      // way the live extractor's drain() does: skip to the next SMPTE UL
      // prefix, drop anything before it, and fail the whole extract on
      // malformed framing or an over-cap declared length instead of stalling
      // forever on a corrupt packet_frame_length()==0.
      std::size_t pos = 0;
      while (pos < reasm.size()) {
        std::span<const std::byte> rest(reasm.data() + pos, reasm.size() - pos);
        const auto ul = std::search(rest.begin(), rest.end(),
                                    kSmpteUlPrefix.begin(), kSmpteUlPrefix.end());
        if (ul == rest.end()) {
          // Retain only the longest actual suffix (up to 3 bytes) that could
          // complete a UL prefix in the next payload; arbitrary trailing
          // garbage must not accumulate.
          std::size_t suffix = 0;
          for (std::size_t n = std::min<std::size_t>(3, rest.size()); n > 0; --n) {
            if (std::equal(rest.end() - n, rest.end(), kSmpteUlPrefix.begin())) {
              suffix = n;
              break;
            }
          }
          pos += rest.size() - suffix;
          break;
        }
        pos += static_cast<std::size_t>(ul - rest.begin());
        rest = std::span<const std::byte>(reasm.data() + pos, reasm.size() - pos);
        auto frame = inspect_packet_frame(rest, kDefaultMaxKlvPacketBytes);
        if (!frame) {  // malformed framing / over-cap declared length
          frame_error = frame.error();
          break;
        }
        if (!*frame) break;  // incomplete: keep the remainder in reasm
        const std::size_t n = **frame;
        on_packet(KlvPacket{rest.subspan(0, n), marks.at(stream_off + pos)});
        pos += n;
      }
      if (pos) {
        reasm.erase(reasm.begin(), reasm.begin() + pos);
        stream_off += pos;
      }
    }
    it->second.clear();
  };

  for (std::size_t i = 0; i + kPkt <= ts.size(); i += kPkt) {
    if (u8(ts, i) != 0x47) continue;  // TS sync byte
    const std::uint8_t b1 = u8(ts, i + 1);
    const std::uint16_t pid = ((b1 & 0x1f) << 8) | u8(ts, i + 2);
    if (klv_pid >= 0 && pid != static_cast<std::uint16_t>(klv_pid)) continue;
    const bool pusi = (b1 >> 6) & 1;
    const std::uint8_t afc = (u8(ts, i + 3) >> 4) & 3;
    std::size_t off = i + 4;
    if (afc & 2) off += 1 + u8(ts, off);  // adaptation field
    if (!(afc & 1) || off > i + kPkt) continue;  // no payload
    auto payload = ts.subspan(off, (i + kPkt) - off);
    if (pusi) {
      flush(pid);
      if (frame_error) break;  // stop at the first framing failure
      pes[pid].assign(payload.begin(), payload.end());
    } else {
      auto it = pes.find(pid);
      if (it != pes.end() && !it->second.empty())
        // GCC 12/13 report a false -Wstringop-overflow here: inlining
        // vector::_M_range_insert's reallocation path loses the iterator
        // bounds, and the diagnostic invents "writing between 2 and SIZE_MAX
        // bytes into a region of size 0" with a self-contradictory offset range
        // ([-SIZE_MAX, -1] into an object of size [1, SIZE_MAX]). Neither half
        // is reachable: `payload` is bounded to [0, 184] by the `off > i + kPkt`
        // guard above, and `it->second` is non-empty by the condition on this
        // very `if`, so the destination is never size 0. Suppress narrowly —
        // re-check when the toolchain moves (issue #41).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
        it->second.insert(it->second.end(), payload.begin(), payload.end());
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    }
  }
  if (!frame_error) {
    for (auto& [pid, _] : pes) flush(pid);
  }
  if (frame_error) return Result<std::monostate>::err(*frame_error);
  return klv_pid >= 0 ? Result<std::monostate>::ok({})
                      : Result<std::monostate>::err(Error::Backend);
}

}  // namespace misbklv
