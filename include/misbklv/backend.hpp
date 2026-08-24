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
inline constexpr std::size_t kDefaultMaxKlvPacketBytes = 16 * 1024 * 1024;

struct ExtractOptions {
  // Cap for one complete KLV frame (UL + BER length + value) during extraction.
  std::size_t max_packet_bytes = kDefaultMaxKlvPacketBytes;
};

// One complete, framed KLV packet delivered by extraction. `bytes` borrow the
// backend's reassembly buffer and are valid ONLY during the handler call — copy
// to retain (ADR 0011 read-borrows boundary).
//
// `pts_ns` is the packet's presentation time **in nanoseconds from the start of
// the source** — the same timeline `Inserter::push()` writes on, so a read →
// edit → write round trip preserves timing (ADR 0021). It is the timestamp of
// the PES that carried the packet's FIRST byte (one PES may hold several
// packets; one packet may span two). `kNoPts` when that PES carried no PTS —
// which is a property of the stream, not a failure: a carrier may contain
// entirely untimed KLV PES, and correlation for those is via the KLV's own
// Item 2 Precision Time Stamp.
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

// ST 0604 Precision Time Stamp SEI handling on the video passthrough path
// (ADR 0024). ST 0604 timestamps live in the video elementary stream, separate
// from the KLV metadata stream; a consumer whose downstream reader wants
// frame-accurate time from the video itself needs them present there.
enum class Sei0604 {
  // Leave the video elementary stream alone. Whatever ST 0604 SEI the source
  // carried comes through unchanged, and a source without any stays without
  // any. This is what video passthrough has always promised (ADR 0020), and
  // the default: we do not edit a caller's video unless asked.
  Preserve,

  // Write an ST 0604 Precision Time Stamp into every access unit we can time,
  // taken from the KLV's ST 0601 item 2 as pushed. Any ST 0604 SEI the source
  // carried is REPLACED, not added to, so a reader finds exactly one Precision
  // Time Stamp per access unit and it agrees with the KLV. Frames with no KLV
  // timestamp within tolerance get no SEI rather than an invented one.
  //
  // H.264 only: `open_insert` fails with `Error::Unsupported` if the source's
  // video is anything else, rather than writing an output whose video silently
  // has no timestamps in it. `Preserve` carries every codec, as it always did.
  Generate,
};

struct InsertConfig {
  std::string sink;        // "file:out.ts" | "udp:host:port" | "srt:uri"
  bool realtime = false;   // pace output against the clock (live sinks; B4)
  // v1 signals 0x06 async (0x15 sync deferred, ADR 0008).

  // Optional video passthrough (ADR 0020, extended ADR 0031). Accepts:
  // - "" (default): KLV-only, no video.
  // - "file:PATH" or bare path that exists: file source via filesrc ! demuxer ! parser (container sniff ADR 0025).
  //   Bare path is backward compatible; prefer "file:" prefix (may be deprecated).
  // - "rtsp[s]://...": live RTSP source via rtspsrc ! rtph264depay/265 ! h264parse/h265parse.
  // - "pipeline:<gst-launch desc>": explicit GstBin escape hatch built via gst_parse_bin_from_description with ghost src pad; the bin must expose a static src pad immediately (no dynamic demuxers like tsdemux requiring pad-added). Use for videotestsrc, udpsrc without demux, or test fixtures.
  // Empty keeps KLV-only pipeline. See ADR 0031.
  //
  // ABNF: video_source = "" / file-path / "file:" path / "rtsp:" uri / "rtsps:" uri / "pipeline:" gst-desc
  // where file-path is a bare path (backward compat; prefer "file:").
  //
  // The stream is parsed, never decoded: whatever codec the source carries
  // (H.264, H.265, MPEG-2, ...) is what the output carries. Every non-video stream in
  // the source is dropped — audio, subtitles, and any KLV the source already
  // has (the caller supplies its own KLV through push()).
  //
  // PTS contract: file video uses time from the start of the source; RTSP and
  // pipeline video use the live branch's running time. BOTH branches must land
  // on that one timeline. So push() must receive a real `pts_ns` on the matching
  // source timeline, in nanoseconds, not wall-clock or epoch. `kNoPts` is a
  // caller error here and is rejected (Error::Unsupported): the synthesized
  // ~30 fps counter has no relation to the source's frame timing and would drift
  // silently.
  //
  // Push order: the muxer waits on the slower pad and the KLV appsrc blocks, so
  // push() applies backpressure as the video branch flows. Push in increasing
  // PTS order, interleaved with the video's progress — do not enqueue metadata
  // far ahead of its corresponding video.
  //
  // realtime + video_source is supported: file source replays on pipeline clock, live source is normal mode (ADR 0031); kNoPts still rejected with video_source.
  std::string video_source;

  // What to do about ST 0604 Precision Time Stamp SEI in the passthrough video
  // (ADR 0024). Only meaningful with `video_source` set.
  Sei0604 sei_0604 = Sei0604::Preserve;

  // Multicast/broadcast knobs for `udp:` sinks (ADR 0031). Each maps to
  // `udpsink` `ttl-mc` / `multicast-iface` / `loop`, with `auto-multicast`
  // forced on for every `udp:` sink. Defaults reproduce today's exact behavior
  // (ttl 1, no pinned interface, loop and auto-multicast on) so existing
  // callers are unchanged. Ignored by `file:` and `srt:` sinks.
  int udp_ttl_mcast = 1;  // udpsink ttl-mc, multicast TTL (0..255; 1 = GStreamer default)
  // udpsink multicast-iface; "" (default) = let the network stack choose. An
  // interface name set here (e.g. "lo", "eth0") must be a valid egress.
  std::string udp_mcast_iface;
  bool udp_loop = true;  // udpsink loop: multicast loopback to the local host
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
  // Ownership-transferring overload for zero-copy emit: by default forwards to
  // the span overload. GStreamer backend overrides to wrap the vector storage
  // without copying (gst_buffer_new_wrapped).
  virtual Result<std::monostate> push(std::vector<std::byte>&& klv_packet,
                                      std::int64_t pts_ns) {
    return push(std::span<const std::byte>(klv_packet.data(), klv_packet.size()),
                pts_ns);
  }
  // EOS + flush + close. `stop` cancels the post-EOS drain early (cooperative,
  // polled from the calling thread — e.g. a SIGINT handler that flips a flag a
  // watcher forwards to a stop_source). A realtime file replay drains its video
  // at wall-clock speed, so without this a Ctrl-C arriving after the KLV is
  // emitted is ignored until the source reaches EOS (or the drain timeout) —
  // see ADR 0032. A default token is never signaled, so finish() runs to the
  // natural EOS, unchanged for every existing caller. On cancellation any
  // partial sink file is discarded (ADR 0022) and finish() returns ok, matching
  // extract()'s cooperative-stop convention (ADR 0019).
  virtual Result<std::monostate> finish(std::stop_token stop = {}) = 0;
  virtual ~Inserter() = default;
};

class MediaBackend {
 public:
  // Drive a demux pipeline; call `on_packet` for each complete KLV packet.
  // Blocking; the handler runs on the backend's thread. `source` is a bare path
  // / "file:PATH" (ends at EOS), live "udp:host:port" (ends after received
  // data then a UDP idle timeout), or live "srt:uri" (no natural idle end).
  // `stop` cancels a live extract early (cooperative, polled from another thread —
  // e.g. a KlvStream consumer that breaks); a default token is never signaled, so
  // extract runs to the natural end (ADR 0019).
  virtual Result<std::monostate> extract(std::string_view source,
                                         const PacketHandler& on_packet,
                                         std::stop_token stop = {},
                                         ExtractOptions options = {}) = 0;
  virtual Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig&) = 0;
  virtual ~MediaBackend() = default;
};

}  // namespace misbklv
