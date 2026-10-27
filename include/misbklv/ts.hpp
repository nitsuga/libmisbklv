// SPDX-License-Identifier: Apache-2.0
// Minimal gstreamer-free MPEG-TS KLV extractor. Finds the KLV elementary PID by
// content (a PES payload starting with a SMPTE UL) and reassembles KLV packets,
// handling BOTH signaling types: 0x06 (KLV directly in the PES) and 0x15 (KLV in
// SMPTE RP 217 metadata AU cells). The 0x15 case is the one stock gstreamer
// tsdemux drops (fork 12); this covers it with no dependency.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

#include "misbklv/backend.hpp"  // PacketHandler, KlvPacket, Result

namespace misbklv {

// Extract KLV packets from a buffer of MPEG-TS bytes; calls `on_packet` per
// framed packet (bytes borrowed during the call, per ADR 0013). Returns a
// Backend error if no KLV PID is found.
//
// Framing is bounded and terminal: a corrupt declared BER length after the UL
// prefix fails the whole extract with BadLength, and a declared frame over the
// 16 MiB reassembly cap (kDefaultMaxKlvPacketBytes) fails with ResourceLimit;
// aggregate pending PES bytes are capped at that frame allowance plus transport
// overhead and fail with the same error. A final incomplete KLV frame fails with
// Truncated. Packets already delivered before any terminal failure stay
// delivered. Garbage between packets is tolerated — framing resyncs on the next
// SMPTE UL prefix and skips anything before it. KLV packets are reassembled
// across PES boundaries, so a packet larger than one PES (above the 16-bit
// PES_packet_length ceiling) or split by a muxer is extracted whole, for 0x06
// and 0x15 alike. RP 217 (0x15) metadata AU cells are extracted whether or not
// fragmented: each cell's bytes are concatenated through the framer and every
// cell in a PES is extracted, not only the first. Fragment service id, sequence
// number and first/middle/last indication are NOT validated, so a lost or
// reordered fragment is not reliably detected: once a first fragment has
// supplied the UL and BER length, the next cells' bytes fill the declared length
// and a corrupt packet can be emitted without a framing error. On the selected
// PID a cell whose declared length overruns the PES, or 1-4 trailing bytes too
// short for a cell header, fails with BadLength. A foreign (unregistered) UL is
// not a framing error here; it surfaces later as UnknownTag from Message::parse.
//
// Single KLV PID: the first PID with a PES in which any cell starts with a UL is
// selected by content; any other KLV PID in the stream is ignored (ADR 0039).
//
// Offline only: this extractor reads both stream_type 0x06 and 0x15, but the
// live gstreamer path (tsdemux) does not surface 0x15 (ADR 0039).
//
// Each packet carries `pts_ns` — nanoseconds from the start of the source,
// measured from the earliest PTS anywhere in `ts` (ADR 0021), or `kNoPts` if
// its PES was untimed. `ts` must therefore be the whole stream: extracting from
// a chunk re-anchors the timeline to that chunk (for input fed in chunks use
// TsKlvStream below).
//
// PTS wrap limit: PTS is 33 bits and wraps about every 26.5 h. A capture that
// crosses the wrap gets `pts_ns` about 26.5 h off, because the origin is the
// minimum PTS; this overload cannot tell you that happened. Use
// extract_ts_klv_timed() below, which reports the wrap (and the raw PTS needed
// to unroll it), or KLV Item 2 (Precision Time Stamp), or split the file before
// extracting. The live path's behavior across the wrap is not specified.
Result<std::monostate> extract_ts_klv(std::span<const std::byte> ts,
                                      const PacketHandler& on_packet);

// Timestamp provenance for one extraction (issue #111). `pts_ns` stays exactly
// what extract_ts_klv() reports; this says where it came from and how far to
// trust it, so a consumer lining KLV up with independently decoded video does
// not re-parse PES/TS headers or guess an offset.
//
// Domain: MPEG-TS PES PTS, 90 kHz ticks, 33 bits (0 .. kPtsModulus-1).
struct TsTimeline {
  inline static constexpr std::int64_t kPtsModulus = std::int64_t{1} << 33;

  enum class Continuity {
    // No PES anywhere in the buffer carried a PTS. Every packet is `kNoPts`.
    // This is a property of the stream; there is nothing to be continuous.
    NoTimestamps,
    // PTS present, and this buffer showed no wrap and no discontinuity
    // indicator, and no PTS step back past the reorder tolerance. Scoped to the
    // buffer: it says nothing about bytes before or after it, and a forward
    // splice (or a backward one inside the tolerance) is not detectable.
    Continuous,
    // A 33-bit wrap, a PTS regression, and/or an adaptation-field
    // discontinuity_indicator was observed. `pts_ns` is NOT a trustworthy
    // continuous timeline across the break; unroll from the per-packet raw PTS
    // and epoch instead, or split.
    Interrupted,
  };

  Continuity continuity = Continuity::NoTimestamps;

  // Source PTS (90 kHz) that `pts_ns == 0` corresponds to: the earliest PTS
  // anywhere in the supplied buffer (all PIDs; ADR 0021). -1 if NoTimestamps.
  // For a buffer that starts mid-stream this is the earliest PTS *in the
  // buffer*, not the stream's start; the library cannot know the difference.
  std::int64_t origin_pts_90k = -1;

  // The buffer straddles a 33-bit PTS wrap (any PID, file order, half-range
  // rule: a backward step of more than 2^32 ticks is a wrap; a forward step that
  // large is a PES reordered from before one, which also marks the straddle).
  bool pts_wrap_observed = false;

  // Wrap epoch of the PES `origin_pts_90k` came from (file-order epoch across
  // all PIDs, 0 until the first wrap). Unrolled origin:
  // origin_pts_90k + origin_pts_epoch * kPtsModulus.
  std::int32_t origin_pts_epoch = 0;

  // A PTS stepped back by more than ~30 s (and not a wrap), which no video
  // reorder produces: a splice or restart. Independent clocks in a multi-program
  // buffer can trip it too; both errors lean toward Interrupted.
  bool pts_regression_observed = false;

  // Any TS packet in the buffer had adaptation-field discontinuity_indicator set.
  bool discontinuity_indicator_observed = false;
};

// A packet plus its raw source timestamp. For a PES without a PTS (or a
// NoTimestamps stream) `source_pts_90k` is -1, matching `packet.pts_ns ==
// kNoPts`.
struct TsKlvPacket {
  KlvPacket packet;
  // Raw 33-bit PTS of the PES that carried the packet's first byte. With the
  // timeline's origin, and absent a wrap: packet.pts_ns == (source_pts_90k -
  // origin_pts_90k) converted to ns (x100000/9, truncated).
  std::int64_t source_pts_90k = -1;
  // Wrap epoch of that PES in the same file-order, all-PID numbering as
  // TsTimeline::origin_pts_epoch (0 until the first wrap, negative for a PES
  // reordered back across it). Unrolled source time is
  // source_pts_90k + pts_epoch * kPtsModulus; subtracting the unrolled origin
  // gives the exact continuous offset that pts_ns cannot across a wrap.
  std::int32_t pts_epoch = 0;
};

// The packet's offset from the timeline origin in 90 kHz ticks, unrolled across
// any 33-bit wrap: (source_pts + epoch * 2^33) - (origin_pts + origin_epoch *
// 2^33). This is the canonical way to combine the two raw/epoch pairs; either
// origin field alone is insufficient after a wrap. Equals `pts_ns` (scaled) when
// no wrap was observed. Empty if the packet is untimed or the timeline has no
// origin (`NoTimestamps`). The video-side origin policy stays the caller's.
inline std::optional<std::int64_t> unrolled_offset_90k(const TsKlvPacket& p, const TsTimeline& t) {
  if (p.source_pts_90k < 0 || t.origin_pts_90k < 0) return std::nullopt;
  return (p.source_pts_90k + std::int64_t{p.pts_epoch} * TsTimeline::kPtsModulus) -
         (t.origin_pts_90k + std::int64_t{t.origin_pts_epoch} * TsTimeline::kPtsModulus);
}

// Same, in nanoseconds (x100000/9, truncated like `pts_ns`).
inline std::optional<std::int64_t> unrolled_offset_ns(const TsKlvPacket& p, const TsTimeline& t) {
  const auto ticks = unrolled_offset_90k(p, t);
  if (!ticks) return std::nullopt;
  return *ticks * 100'000 / 9;
}

using TsPacketHandler = std::function<void(const TsKlvPacket&)>;

// Same extraction as extract_ts_klv() (identical framing, errors and `pts_ns`),
// plus provenance. `timeline` is filled whenever the call returns, including
// on error (it reflects the whole buffer, which is scanned up front); it may
// be null. Whole-stream input gives the stream's own origin; a buffer that
// begins mid-stream anchors at its own earliest PTS, and origin_pts_90k says
// which source tick that was.
Result<std::monostate> extract_ts_klv_timed(std::span<const std::byte> ts,
                                            const TsPacketHandler& on_packet, TsTimeline* timeline);

// Incremental extraction for inputs too large to hold as one span (issue #115). The
// stream is fed in arbitrary chunks (a chunk need not be packet-aligned; the
// 188-byte grid starts at the first byte fed, like the span overloads) and is read
// once, front to back. Memory is bounded by the pending PES bytes (the same
// allowance as the span overloads), the PES starts awaiting a timestamp (a start
// is resolved or expired within 16384 packets, which bounds how many can wait), and the wrap epochs
// of PES that may still be framed, not by the input length; see Stats. After a
// framing failure nothing is retained for framing, so feeding on stays bounded.
//
// Same extraction as extract_ts_klv_timed() (identical framing, PID selection,
// errors, raw `source_pts_90k` and `pts_epoch` per packet, and the same final
// TsTimeline), with two differences that follow from reading once:
//  - The origin is the earliest PTS anywhere in the stream, which is unknown until
//    the end, so `packet.pts_ns` is always `kNoPts`. Compute offsets after
//    finish() with unrolled_offset_90k()/unrolled_offset_ns() and the final
//    timeline, as extract_ts_klv_timed() callers already can.
//  - After a terminal framing failure feed() keeps returning that error and stops
//    extracting, but keeps scanning timestamps so finish() still fills the
//    timeline for the whole stream, as the span overloads do. Callers that do not
//    need the timeline may stop feeding.
//
// Wrap epochs are numbered in file order of PES starts, as in the span overloads.
// A PES whose 14 header bytes span several packets resolves later than its start;
// delivery of KLV packets waits for every earlier start to resolve, so epochs
// match the span overloads. That wait is bounded: a start whose header bytes have
// not all arrived within 16384 transport packets (about 3 MB) of its start is
// treated as untimed. This is the one place the result can differ from the span
// overloads, which wait for that PID's next packets however far away they are,
// and the difference is not confined to that PES: its PTS is never shown to the
// wrap unroller, so it can change the wrap epochs of later packets and the
// timeline (wrap flag, origin), and if it is a KLV PES it is delivered with
// `source_pts_90k == -1` (untimed). It only matters for a PES header split
// across packets that far apart, or a damaged start, and it keeps one such start
// from stalling every later delivery.
// Pending PES bytes beyond the allowance above fail with ResourceLimit.
//
// Not thread-safe; handler calls happen on the feeding thread.
class TsKlvStream {
 public:
  explicit TsKlvStream(TsPacketHandler on_packet);
  ~TsKlvStream();
  TsKlvStream(const TsKlvStream&) = delete;
  TsKlvStream& operator=(const TsKlvStream&) = delete;

  // Feeds the next bytes. Returns the first terminal framing failure so far, if
  // any (sticky), otherwise ok. Packets completed by these bytes are delivered
  // before it returns.
  Result<std::monostate> feed(std::span<const std::byte> ts);

  // Ends the stream: resolves pending timestamps, flushes the last PES of each PID
  // and fills `timeline` (may be null). Returns what extract_ts_klv_timed() would
  // for the same bytes: the first framing failure, Truncated for a final
  // incomplete frame, Backend when no KLV PID was found, else ok. Trailing bytes
  // short of a packet are ignored, as in the span overloads. No further calls.
  Result<std::monostate> finish(TsTimeline* timeline);

  // Bookkeeping sizes, for tests and diagnostics: all are bounded independently of the input
  // length.
  struct Stats {
    std::size_t unresolved_starts;  // PES starts not yet observed (unresolved, or queued behind an
                                    // older start)
    std::size_t deferred_jobs;      // finished PES waiting for an earlier start to resolve
    std::size_t epoch_entries;      // wrap epochs retained for PES that may still be framed
    std::size_t pending_pes_bytes;
  };
  Stats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace misbklv
