// SPDX-License-Identifier: Apache-2.0
// MediaBackend — the interface between the KLV core and MPEG-TS I/O (ADR 0013).
// Byte-level: it moves whole KLV packets, never decodes them. Core-only header
// (no gstreamer); GstBackend / MockBackend implement it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>

#include "misbklv/types.hpp"

namespace misbklv {

inline constexpr std::int64_t kNoPts = -1;

// One complete, framed KLV packet delivered by extraction. `bytes` borrow the
// backend's reassembly buffer and are valid ONLY during the handler call — copy
// to retain (ADR 0011 read-borrows boundary).
//
// `pts_ns` is the packet's presentation time **in nanoseconds from the start of
// the source** — the same timeline `Inserter::push()` writes on, so a read →
// edit → write round trip preserves timing (ADR 0021). It is the timestamp of
// the PES that carried the packet's FIRST byte (one PES may hold several
// packets; one packet may span two). `kNoPts` when that PES carried no PTS —
// which is a property of the stream, not a failure: real captures exist whose
// KLV PES are entirely untimed (`data/Day Flight.mpg`), and correlation for
// those is via the KLV's own Item 2 Precision Time Stamp.
//
// "Start of the source" is established per extractor and can differ by about a
// frame on a stream that begins mid-PES: the gstreamer backend uses the
// demuxer's running time (its segment spans the whole program), while
// `extract_ts_klv` subtracts the earliest PTS in the buffer it was handed.
// Intervals are exact in both.
struct KlvPacket {
  std::span<const std::byte> bytes;  // parse_packet-able
  std::int64_t pts_ns = kNoPts;      // ns from the start of the source, or kNoPts
};

using PacketHandler = std::function<void(const KlvPacket&)>;

struct InsertConfig {
  std::string sink;        // "file:out.ts" | "udp:host:port" | "srt:uri"
  bool realtime = false;   // pace output against the clock (live sinks; B4)
  // v1 signals 0x06 async (0x15 sync deferred, ADR 0008).

  // Optional video passthrough (ADR 0020). A bare path or "file:PATH" to a media
  // file whose *first* video elementary stream is re-muxed, UNCHANGED, alongside
  // the KLV. Empty (the default) keeps the KLV-only pipeline exactly as it was.
  //
  // The stream is parsed, never decoded: whatever codec the source carries
  // (H.264, H.265, ...) is what the output carries. Every non-video stream in
  // the source is dropped — audio, subtitles, and any KLV the source already
  // has (the caller supplies its own KLV through push()).
  //
  // PTS contract: the video branch is timestamped from the source file, running
  // from zero at the start of that file, and BOTH branches must land on that one
  // timeline. So push() must be called with a real `pts_ns` on the same timeline
  // — presentation time from the start of the source, in nanoseconds, not
  // wall-clock and not epoch. `kNoPts` is a caller error here and is rejected
  // (Error::Unsupported): the synthesized ~30 fps counter has no relation to the
  // source's frame timing and would drift silently.
  //
  // Push order: the muxer waits on the slower pad and the KLV appsrc blocks, so
  // push() applies backpressure as the video branch flows. Push in increasing
  // PTS order, interleaved with the video's progress — do not try to push a whole
  // file's worth of KLV before the video has started.
  //
  // Not supported with `realtime` (rejected: Error::Unsupported) — the video
  // branch is a file source and clock-paced output with it is unexercised.
  std::string video_source;
};

// Real-time push session (ADR 0013). push() blocks on sink backpressure.
//
// No output file unless the session succeeds (ADR 0022). For a `file:` sink,
// the only thing that leaves an output file behind is a `finish()` that returns
// ok. A failing `finish()`, or destroying the Inserter without one, removes the
// sink file — but only if this session created it: a file that already existed
// at that path is the caller's and is never deleted (its old *contents* are
// still gone, since opening a file sink truncates).
class Inserter {
 public:
  // `pts_ns`: presentation time in nanoseconds from the start of the source
  // (the same timeline extraction reports on — KlvPacket::pts_ns). `kNoPts`
  // synthesizes a ~30 fps counter on a KLV-only pipeline, and is rejected when
  // the config has a `video_source` (ADR 0020).
  virtual Result<std::monostate> push(std::span<const std::byte> klv_packet,
                                      std::int64_t pts_ns) = 0;
  virtual Result<std::monostate> finish() = 0;  // EOS + flush + close
  virtual ~Inserter() = default;
};

class MediaBackend {
 public:
  // Drive a demux pipeline; call `on_packet` for each complete KLV packet.
  // Blocking; the handler runs on the backend's thread. `source` is a bare path
  // / "file:PATH" (ends at EOS) or a live "udp:host:port" / "srt:uri" (ends when
  // the source goes idle after delivering data — no EOS crosses the network, B4).
  // `stop` cancels a live extract early (cooperative, polled from another thread —
  // e.g. a KlvStream consumer that breaks); a default token is never signaled, so
  // extract runs to the natural end (ADR 0019).
  virtual Result<std::monostate> extract(std::string_view source,
                                         const PacketHandler& on_packet,
                                         std::stop_token stop = {}) = 0;
  virtual Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig&) = 0;
  virtual ~MediaBackend() = default;
};

}  // namespace misbklv
