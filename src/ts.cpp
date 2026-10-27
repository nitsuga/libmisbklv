// SPDX-License-Identifier: Apache-2.0
#include "misbklv/ts.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

#include "misbklv/backend.hpp"  // kNoPts
#include "klv_framer.hpp"

namespace misbklv {
namespace {

constexpr std::uint8_t kSmpteUl[4] = {0x06, 0x0e, 0x2b, 0x34};
constexpr std::size_t kPkt = 188;
// Before the KLV PID is known, retain at most one maximum KLV frame plus the
// largest legal PES header and one RP 217 AU-cell header.
// ponytail: one shared budget can reject an exact-cap KLV frame while another
// PID is pending; preselect the KLV PID earlier if real multiplexes need that edge.
constexpr std::size_t kMaxPendingPesBytes = kDefaultMaxKlvPacketBytes + 264 + 5;

std::uint8_t u8(std::span<const std::byte> s, std::size_t i) {
  return std::to_integer<std::uint8_t>(s[i]);
}

bool starts_with_ul(std::span<const std::byte> s) {
  return s.size() >= 4 && u8(s, 0) == kSmpteUl[0] && u8(s, 1) == kSmpteUl[1] &&
         u8(s, 2) == kSmpteUl[2] && u8(s, 3) == kSmpteUl[3];
}

// A PES unwrapped into its KLV payload pieces, in order; never empty (a lone
// empty span if not KLV/parseable). `malformed` is set for a 0xFC PES whose AU
// cells do not tile the payload exactly (see unwrap_pes); `cells` then holds
// only the well-formed cells before the defect.
struct PesCells {
  std::vector<std::span<const std::byte>> cells;
  bool malformed = false;
};

// Unwrap a full PES packet. A 0x06 PES is one piece. A 0xFC PES (SMPTE RP 217)
// holds one or more metadata AU cells, each a 5-byte header whose last two bytes
// are the big-endian data length, then that many bytes; every cell is one piece.
// The cell_fragmentation_indication bits are ignored: each fragment carries its
// own header and length, and the framer reassembles them across cells and PES.
// A declared length past the PES end, or 1-4 trailing bytes too short for a cell
// header, is malformed (the caller raises it only for the selected PID).
PesCells unwrap_pes(std::span<const std::byte> pes) {
  constexpr std::span<const std::byte> kNone;
  if (pes.size() < 9 || u8(pes, 0) != 0 || u8(pes, 1) != 0 || u8(pes, 2) != 1) return {{kNone}};
  const std::uint8_t stream_id = u8(pes, 3);
  const std::size_t pes_len = (u8(pes, 4) << 8) | u8(pes, 5);
  const std::size_t poff = ((u8(pes, 6) & 0xC0) == 0x80) ? 9 + u8(pes, 8) : 6;
  const std::size_t end =
      (pes_len > 0) ? std::min<std::size_t>(6 + pes_len, pes.size()) : pes.size();
  if (poff > end) return {{kNone}};
  auto payload = pes.subspan(poff, end - poff);
  if (stream_id != 0xFC) return {{payload}};  // 0x06: KLV directly in the PES
  PesCells out;
  while (payload.size() >= 5) {
    const std::size_t clen = (u8(payload, 3) << 8) | u8(payload, 4);
    if (clen > payload.size() - 5) {
      out.malformed = true;
      return out;
    }
    out.cells.push_back(payload.subspan(5, clen));
    payload = payload.subspan(5 + clen);
  }
  if (!payload.empty()) out.malformed = true;
  if (out.cells.empty()) out.cells.push_back(kNone);
  return out;
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

// Unrolls 33-bit PTS values observed in file order into a wrap epoch.
// Half-range rule: a step back of more than 2^32 ticks is a wrap (epoch++); a
// step forward of more than 2^32 is a PES reordered from before a wrap (reported
// one epoch back, running state untouched). Both mean the buffer straddles a
// wrap, so both set `wrapped()`: a buffer that starts just after a wrap but still
// holds a pre-wrap reordered frame is not continuous. Anything smaller is
// ordinary jitter/reorder, except a step back past `kReorderTolerance`, which no
// legitimate reorder produces and is reported as a regression.
class PtsUnroller {
 public:
  // About 30 s: far beyond any video reorder depth, short of any real splice.
  static constexpr std::int64_t kReorderTolerance = 30 * 90'000;

  std::int32_t observe(std::int64_t pts) {
    if (prev_ < 0) {
      prev_ = pts;
      return 0;
    }
    constexpr std::int64_t kHalf = TsTimeline::kPtsModulus / 2;
    const std::int64_t delta = pts - prev_;
    if (delta < -kHalf) {
      ++epoch_;
      wrapped_ = true;
    } else if (delta > kHalf) {
      wrapped_ = true;
      return epoch_ - 1;
    } else if (delta < -kReorderTolerance) {
      regressed_ = true;
    }
    prev_ = pts;
    return epoch_;
  }
  bool wrapped() const { return wrapped_; }
  bool regressed() const { return regressed_; }

 private:
  std::int64_t prev_ = -1;
  std::int32_t epoch_ = 0;
  bool wrapped_ = false;
  bool regressed_ = false;
};

// The PTS of the PES that starts in TS packet `i`, in 90 kHz ticks, or -1. The
// 14 header bytes may not fit in the first packet (a large adaptation field), so
// they are gathered across that PID's following packets exactly as extraction
// reassembles the PES; scan and extraction must agree on which PES are timed.
std::int64_t pes_start_pts_90k(std::span<const std::byte> ts, std::size_t i) {
  const std::uint16_t pid = ((u8(ts, i + 1) & 0x1f) << 8) | u8(ts, i + 2);
  std::array<std::byte, 14> head{};
  std::size_t n = 0;
  for (std::size_t j = i; j + kPkt <= ts.size() && n < head.size(); j += kPkt) {
    if (u8(ts, j) != 0x47) continue;
    const std::uint16_t jpid = ((u8(ts, j + 1) & 0x1f) << 8) | u8(ts, j + 2);
    if (jpid != pid) continue;
    if (j != i && ((u8(ts, j + 1) >> 6) & 1)) break;  // next PES began first
    const std::uint8_t afc = (u8(ts, j + 3) >> 4) & 3;
    std::size_t off = j + 4;
    if (afc & 2) off += 1 + u8(ts, off);
    if (!(afc & 1) || off > j + kPkt) continue;
    const std::size_t take = std::min(head.size() - n, j + kPkt - off);
    std::copy_n(ts.begin() + off, take, head.begin() + n);
    n += take;
  }
  if (n < head.size()) return -1;
  if (u8(head, 0) != 0 || u8(head, 1) != 0 || u8(head, 2) != 1) return -1;
  return pes_pts_90k(head);
}

// Everything the timestamp pre-pass learns. A cheap header-only pass — it looks
// at the first 14 bytes of each PES, never at payload.
//
// `origin_90k` is the earliest PTS anywhere in the buffer: the start of the
// source's presentation, which is what the insert path's timeline is also
// anchored to (ADR 0021). The minimum, not the first in file order: with
// reordered video the first PES in the file is not the earliest presentation
// time, and picking it would shift every reported timestamp by the reorder
// delay.
//
// The origin is still the raw minimum, so a wrap leaves `pts_ns` ~26.5 h off, as
// it always did; the flags and epochs are what make that visible (issue #111).
// ponytail: unroll the origin (min over raw + epoch * 2^33) to make pts_ns
// itself wrap-correct; held back to keep pts_ns byte-compatible.
struct ScanResult {
  std::int64_t origin_90k = -1;
  std::int32_t origin_epoch = 0;
  bool wrapped = false;
  bool regressed = false;
  bool discontinuity = false;
  bool any_pts = false;
  // TS packet offset of a PES start -> wrap epoch, for PES with a nonzero epoch
  // only (empty unless the buffer wraps).
  std::unordered_map<std::size_t, std::int32_t> epoch_at;
};

ScanResult scan_timestamps(std::span<const std::byte> ts) {
  ScanResult r;
  PtsUnroller unroller;
  for (std::size_t i = 0; i + kPkt <= ts.size(); i += kPkt) {
    if (u8(ts, i) != 0x47) continue;
    const std::uint8_t afc = (u8(ts, i + 3) >> 4) & 3;
    if ((afc & 2) && u8(ts, i + 4) >= 1 && (u8(ts, i + 5) & 0x80)) r.discontinuity = true;
    if (!((u8(ts, i + 1) >> 6) & 1)) continue;  // not a PES start
    const std::int64_t pts = pes_start_pts_90k(ts, i);
    if (pts < 0) continue;
    r.any_pts = true;
    const std::int32_t epoch = unroller.observe(pts);
    if (epoch != 0) r.epoch_at[i] = epoch;
    if (r.origin_90k < 0 || pts < r.origin_90k) {
      r.origin_90k = pts;
      r.origin_epoch = epoch;
    }
  }
  r.wrapped = unroller.wrapped();
  r.regressed = unroller.regressed();
  return r;
}

}  // namespace

Result<std::monostate> extract_ts_klv(std::span<const std::byte> ts,
                                      const PacketHandler& on_packet) {
  return extract_ts_klv_timed(ts, [&](const TsKlvPacket& p) { on_packet(p.packet); }, nullptr);
}

Result<std::monostate> extract_ts_klv_timed(std::span<const std::byte> ts,
                                            const TsPacketHandler& on_packet,
                                            TsTimeline* timeline) {
  std::unordered_map<std::uint16_t, std::vector<std::byte>> pes;  // PID -> current PES
  int klv_pid = -1;
  const ScanResult scan = scan_timestamps(ts);
  const std::int64_t origin_90k = scan.origin_90k;
  if (timeline) {
    timeline->origin_pts_90k = origin_90k;
    timeline->origin_pts_epoch = scan.origin_epoch;
    timeline->pts_wrap_observed = scan.wrapped;
    timeline->pts_regression_observed = scan.regressed;
    timeline->discontinuity_indicator_observed = scan.discontinuity;
    timeline->continuity = !scan.any_pts ? TsTimeline::Continuity::NoTimestamps
                           : (scan.wrapped || scan.regressed || scan.discontinuity)
                               ? TsTimeline::Continuity::Interrupted
                               : TsTimeline::Continuity::Continuous;
  }
  std::unordered_map<std::uint16_t, std::size_t> pes_off;  // PID -> TS offset of current PES
  detail::KlvFramer framer;
  const PacketHandler deliver = [&](const KlvPacket& kp) {
    const auto aux = framer.packet_aux();
    on_packet(TsKlvPacket{kp, aux.source_pts_90k, aux.pts_epoch});
  };
  std::optional<Error> frame_error;  // first framing failure, returned at the end
  std::size_t pending_pes_bytes = 0;
  auto release = [&](std::vector<std::byte>& bytes) {
    pending_pes_bytes -= bytes.size();
    std::vector<std::byte>().swap(bytes);
  };

  auto flush = [&](std::uint16_t pid) {
    auto it = pes.find(pid);
    if (it == pes.end() || it->second.empty()) return;
    const auto [cells, malformed] = unwrap_pes(it->second);
    // Any cell may start the KLV packet: a PES can open with a continuation.
    if (klv_pid < 0 && std::any_of(cells.begin(), cells.end(), starts_with_ul)) {
      klv_pid = pid;
      // Candidate buffers cannot contribute after the first KLV PID is found.
      for (auto& [candidate, bytes] : pes) {
        if (candidate == pid) continue;
        release(bytes);
      }
    }
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
      const std::int64_t pts_ns =
          (pts90 < 0 || origin_90k < 0) ? kNoPts : (pts90 - origin_90k) * 100'000 / 9;
      // ponytail: fragment cells are concatenated as-is; service id, sequence
      // number and first/middle/last transitions are not validated, so a lost or
      // reordered fragment is not reliably detected: after a first fragment
      // supplies the UL and BER length, the next cells' bytes fill the declared
      // length and a corrupt packet can be emitted with no framing error.
      // Upgrade path: validate the cell
      // sequence byte and indication transitions per PID once the RP 217 layout
      // is confirmed from a source in references/ (ADR 0039).
      std::int32_t epoch = 0;
      if (const auto e = scan.epoch_at.find(pes_off[pid]); e != scan.epoch_at.end())
        epoch = e->second;
      for (const auto cell : cells) {
        frame_error = framer.feed(cell, pts_ns, deliver, {pts90, epoch});
        if (frame_error) break;
      }
      if (!frame_error && malformed) frame_error = Error::BadLength;
    }
    pending_pes_bytes -= it->second.size();
    if (klv_pid >= 0 && pid == static_cast<std::uint16_t>(klv_pid))
      it->second.clear();  // reuse the selected PID's bounded PES allocation
    else
      std::vector<std::byte>().swap(it->second);
  };

  for (std::size_t i = 0; i + kPkt <= ts.size(); i += kPkt) {
    if (u8(ts, i) != 0x47) continue;  // TS sync byte
    const std::uint8_t b1 = u8(ts, i + 1);
    const std::uint16_t pid = ((b1 & 0x1f) << 8) | u8(ts, i + 2);
    if (klv_pid >= 0 && pid != static_cast<std::uint16_t>(klv_pid)) continue;
    const bool pusi = (b1 >> 6) & 1;
    const std::uint8_t afc = (u8(ts, i + 3) >> 4) & 3;
    std::size_t off = i + 4;
    if (afc & 2) off += 1 + u8(ts, off);         // adaptation field
    if (!(afc & 1) || off > i + kPkt) continue;  // no payload
    auto payload = ts.subspan(off, (i + kPkt) - off);
    if (pusi) {
      flush(pid);
      if (frame_error) break;  // stop at the first framing failure
      if (payload.size() > kMaxPendingPesBytes - pending_pes_bytes) {
        frame_error = Error::ResourceLimit;
        break;
      }
      pes_off[pid] = i;
      auto& current = pes[pid];
      current.assign(payload.begin(), payload.end());
      pending_pes_bytes += current.size();
    } else {
      auto it = pes.find(pid);
      if (it != pes.end() && !it->second.empty()) {
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
        if (payload.size() > kMaxPendingPesBytes - pending_pes_bytes) {
          frame_error = Error::ResourceLimit;
        } else {
          it->second.insert(it->second.end(), payload.begin(), payload.end());
          pending_pes_bytes += payload.size();
        }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
      }
      if (frame_error) break;
    }
  }
  if (!frame_error) {
    for (auto& [pid, _] : pes) flush(pid);
    if (!frame_error && !framer.remainder().empty()) frame_error = Error::Truncated;
  }
  if (frame_error) return Result<std::monostate>::err(*frame_error);
  return klv_pid >= 0 ? Result<std::monostate>::ok({})
                      : Result<std::monostate>::err(Error::Backend);
}

// ---------------------------------------------------------------------------
// Incremental extraction. The logic below is deliberately the span overload's,
// split so each step needs only the bytes seen so far: the timestamp pre-pass
// becomes per-PID header accumulators resolved in PES-start order, and the PES
// flush is deferred (in order) until the wrap epoch of its start is known.

struct TsKlvStream::Impl {
  // A PES start whose 14 header bytes have not arrived this many transport packets later is treated
  // as untimed. The span overload waits for that PID's next packets however far away they are;
  // waiting here would stall every later delivery behind one damaged packet.
  static constexpr std::uint64_t kHeaderWindowPackets = 16384;

  struct Head {
    bool active = false;  // a PES start whose 14 header bytes are still being gathered
    std::uint64_t seq = 0;
    std::array<std::byte, 14> bytes{};
    std::size_t n = 0;
  };
  struct Pending {
    bool resolved = false;
    std::int64_t pts = -1;
    std::uint16_t pid = 0;
    std::uint64_t start_slot = 0;
    bool framed = true;  // extraction can own this start (it has payload bytes); otherwise no job
                         // will use its epoch
  };
  struct EpochEntry {
    std::int32_t epoch;
    std::uint16_t pid;
  };
  struct Job {
    std::uint16_t pid;
    std::vector<std::byte> bytes;
    std::uint64_t seq;
  };

  explicit Impl(TsPacketHandler handler) : on_packet(std::move(handler)) {}

  TsPacketHandler on_packet;
  std::array<std::byte, kPkt> carry{};
  std::size_t carry_n = 0;

  // Timestamp scan.
  std::array<Head, 8192> heads;
  std::deque<Pending> pending;  // PES starts [drained, started), oldest first
  std::uint64_t started = 0;
  std::uint64_t drained = 0;  // every start below this has been observed
  PtsUnroller unroller;
  // Nonzero epochs of PES starts that may still be framed (the open PES of a candidate or KLV PID,
  // or a queued job); an entry is erased when its PES is framed or dropped. kUntimed marks a start
  // that expired unresolved: its PES is framed with no PTS rather than with a PTS and a guessed
  // epoch.
  static constexpr std::int32_t kUntimed = INT32_MIN;
  std::unordered_map<std::uint64_t, EpochEntry> epoch_at;
  std::uint64_t slot = 0;
  std::int64_t origin_90k = -1;
  std::int32_t origin_epoch = 0;
  bool any_pts = false;
  bool discontinuity = false;

  // Extraction.
  std::unordered_map<std::uint16_t, std::vector<std::byte>> pes;
  std::unordered_map<std::uint16_t, std::uint64_t> pes_seq;
  std::deque<Job> jobs;
  int klv_pid = -1;
  detail::KlvFramer framer;
  std::optional<Error> frame_error;
  std::size_t pending_pes_bytes = 0;
  std::size_t expired_starts = 0;

  // Extraction is terminal after a failure: release what it held. Timestamp scanning continues (so
  // finish() still fills the whole-stream timeline) but retains nothing for framing, so state stays
  // bounded however much is fed. A job running when this is called subtracts its own bytes when it
  // returns.
  void fail(Error e) {
    if (!frame_error) frame_error = e;
    for (const Job& job : jobs) pending_pes_bytes -= job.bytes.size();
    jobs.clear();
    for (auto& entry : pes) release(entry.second);
    epoch_at.clear();
  }

  void resolve(std::uint64_t seq, std::int64_t pts) {
    const auto index = static_cast<std::size_t>(seq - drained);
    pending[index].resolved = true;
    pending[index].pts = pts;
    while (!pending.empty() && pending.front().resolved) {
      const std::int64_t value = pending.front().pts;
      const std::uint16_t pid = pending.front().pid;
      const bool framed = pending.front().framed;
      pending.pop_front();
      if (value >= 0) {
        any_pts = true;
        const std::int32_t epoch = unroller.observe(value);
        if (epoch != 0 && framed && !frame_error &&
            (klv_pid < 0 || pid == static_cast<std::uint16_t>(klv_pid)))
          epoch_at[drained] = {epoch, pid};
        if (origin_90k < 0 || value < origin_90k) {
          origin_90k = value;
          origin_epoch = epoch;
        }
      }
      ++drained;
    }
  }

  void release(std::vector<std::byte>& bytes) {
    pending_pes_bytes -= bytes.size();
    std::vector<std::byte>().swap(bytes);
  }

  void run(Job& job) {
    if (job.bytes.empty()) return;
    const auto [cells, malformed] = unwrap_pes(job.bytes);
    if (klv_pid < 0 && std::any_of(cells.begin(), cells.end(), starts_with_ul)) {
      klv_pid = job.pid;
      std::erase_if(epoch_at, [&](const auto& entry) { return entry.second.pid != job.pid; });
      for (auto& [candidate, bytes] : pes) {
        if (candidate == job.pid) continue;
        release(bytes);
      }
    }
    if (klv_pid >= 0 && job.pid == static_cast<std::uint16_t>(klv_pid) && !frame_error) {
      std::int64_t pts90 = pes_pts_90k(job.bytes);
      std::int32_t epoch = 0;
      if (const auto e = epoch_at.find(job.seq); e != epoch_at.end()) {
        if (e->second.epoch == kUntimed)
          pts90 = -1;
        else
          epoch = e->second.epoch;
      }
      const PacketHandler deliver = [&](const KlvPacket& kp) {
        const auto aux = framer.packet_aux();
        on_packet(TsKlvPacket{kp, aux.source_pts_90k, aux.pts_epoch});
      };
      for (const auto cell : cells) {
        const auto error = framer.feed(cell, kNoPts, deliver, {pts90, epoch});
        if (error) {
          fail(*error);
          break;
        }
      }
      if (!frame_error && malformed) fail(Error::BadLength);
    }
    epoch_at.erase(job.seq);
    pending_pes_bytes -= job.bytes.size();
  }

  void run_ready() {
    while (!jobs.empty() && !frame_error && jobs.front().seq < drained) {
      Job job = std::move(jobs.front());
      jobs.pop_front();
      run(job);
    }
  }

  void flush(std::uint16_t pid) {
    auto it = pes.find(pid);
    if (it == pes.end() || it->second.empty()) {
      if (const auto seq = pes_seq.find(pid); seq != pes_seq.end()) epoch_at.erase(seq->second);
      return;
    }
    Job job{pid, std::move(it->second), pes_seq[pid]};
    it->second = std::vector<std::byte>();
    if (!jobs.empty() || job.seq >= drained) {
      jobs.push_back(std::move(job));
    } else {
      run(job);
    }
  }

  // Resolves, as untimed, starts still waiting for header bytes long after they began.
  void expire_stale_starts() {
    while (!pending.empty() && !pending.front().resolved &&
           slot - pending.front().start_slot >= kHeaderWindowPackets) {
      const Pending& front = pending.front();
      ++expired_starts;
      Head& head = heads[front.pid];
      if (head.active && head.seq == drained) head.active = false;
      if (front.framed && !frame_error &&
          (klv_pid < 0 || front.pid == static_cast<std::uint16_t>(klv_pid)))
        epoch_at[drained] = {kUntimed, front.pid};
      resolve(drained, -1);
    }
  }

  void packet(std::span<const std::byte> p) {
    ++slot;
    if (u8(p, 0) != 0x47) return;
    const std::uint8_t b1 = u8(p, 1);
    const std::uint16_t pid = ((b1 & 0x1f) << 8) | u8(p, 2);
    const bool pusi = (b1 >> 6) & 1;
    const std::uint8_t afc = (u8(p, 3) >> 4) & 3;
    if ((afc & 2) && u8(p, 4) >= 1 && (u8(p, 5) & 0x80)) discontinuity = true;
    std::size_t off = 4;
    if (afc & 2) off += 1 + u8(p, 4);
    const bool has_payload = (afc & 1) && off <= kPkt;

    {
      Head& head = heads[pid];
      if (pusi) {
        if (head.active) resolve(head.seq, -1);  // the next PES began before 14 bytes arrived
        head = Head{};
        head.active = true;
        head.seq = started++;
        pending.push_back({false, -1, pid, slot, has_payload && off < kPkt});
      }
      if (head.active && has_payload) {
        const std::size_t take = std::min(head.bytes.size() - head.n, kPkt - off);
        std::copy_n(p.begin() + off, take, head.bytes.begin() + head.n);
        head.n += take;
        if (head.n == head.bytes.size()) {
          head.active = false;
          const bool start_code =
              u8(head.bytes, 0) == 0 && u8(head.bytes, 1) == 0 && u8(head.bytes, 2) == 1;
          resolve(head.seq, start_code ? pes_pts_90k(head.bytes) : -1);
        }
      }
      expire_stale_starts();
    }

    if (frame_error) return;
    run_ready();
    if (frame_error) return;
    if (klv_pid >= 0 && pid != static_cast<std::uint16_t>(klv_pid)) return;
    if (!has_payload) return;
    const auto payload = p.subspan(off);
    if (pusi) {
      flush(pid);
      if (frame_error) return;
      if (payload.size() > kMaxPendingPesBytes - pending_pes_bytes) {
        fail(Error::ResourceLimit);
        return;
      }
      pes_seq[pid] = started - 1;
      auto& current = pes[pid];
      current.assign(payload.begin(), payload.end());
      pending_pes_bytes += current.size();
    } else {
      auto it = pes.find(pid);
      if (it != pes.end() && !it->second.empty()) {
        if (payload.size() > kMaxPendingPesBytes - pending_pes_bytes) {
          fail(Error::ResourceLimit);
        } else {
          // Same false -Wstringop-overflow as the span overload (issue #41): payload is at most
          // 184 bytes and the destination is non-empty.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
          it->second.insert(it->second.end(), payload.begin(), payload.end());
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
          pending_pes_bytes += payload.size();
        }
      }
    }
  }
};

TsKlvStream::TsKlvStream(TsPacketHandler on_packet)
    : impl_(std::make_unique<Impl>(std::move(on_packet))) {}
TsKlvStream::~TsKlvStream() = default;

TsKlvStream::Stats TsKlvStream::stats() const {
  const Impl& s = *impl_;
  return {s.pending.size(), s.jobs.size(), s.epoch_at.size(), s.pending_pes_bytes,
          s.expired_starts};
}

Result<std::monostate> TsKlvStream::feed(std::span<const std::byte> ts) {
  Impl& s = *impl_;
  if (s.carry_n) {
    const std::size_t take = std::min(kPkt - s.carry_n, ts.size());
    std::copy_n(ts.begin(), take, s.carry.begin() + s.carry_n);
    s.carry_n += take;
    ts = ts.subspan(take);
    if (s.carry_n < kPkt)
      return s.frame_error ? Result<std::monostate>::err(*s.frame_error)
                           : Result<std::monostate>::ok({});
    s.packet(s.carry);
    s.carry_n = 0;
  }
  while (ts.size() >= kPkt) {
    s.packet(ts.first(kPkt));
    ts = ts.subspan(kPkt);
  }
  std::copy(ts.begin(), ts.end(), s.carry.begin());
  s.carry_n = ts.size();
  return s.frame_error ? Result<std::monostate>::err(*s.frame_error)
                       : Result<std::monostate>::ok({});
}

Result<std::monostate> TsKlvStream::finish(TsTimeline* timeline) {
  Impl& s = *impl_;
  for (std::size_t pid = 0; pid < s.heads.size(); ++pid) {
    auto& head = s.heads[pid];
    if (head.active) {
      head.active = false;
      s.resolve(head.seq, -1);
    }
  }
  s.run_ready();
  if (!s.frame_error) {
    for (auto& [pid, _] : s.pes) s.flush(pid);
    s.run_ready();
    if (!s.frame_error && !s.framer.remainder().empty()) s.frame_error = Error::Truncated;
  }
  if (timeline) {
    timeline->origin_pts_90k = s.origin_90k;
    timeline->origin_pts_epoch = s.origin_epoch;
    timeline->pts_wrap_observed = s.unroller.wrapped();
    timeline->pts_regression_observed = s.unroller.regressed();
    timeline->discontinuity_indicator_observed = s.discontinuity;
    timeline->continuity = !s.any_pts ? TsTimeline::Continuity::NoTimestamps
                           : (s.unroller.wrapped() || s.unroller.regressed() || s.discontinuity)
                               ? TsTimeline::Continuity::Interrupted
                               : TsTimeline::Continuity::Continuous;
  }
  if (s.frame_error) return Result<std::monostate>::err(*s.frame_error);
  return s.klv_pid >= 0 ? Result<std::monostate>::ok({})
                        : Result<std::monostate>::err(Error::Backend);
}

}  // namespace misbklv
