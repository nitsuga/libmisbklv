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
#include <string>
#include <string_view>
#include <variant>

#include "misbklv/types.hpp"

namespace misbklv {

inline constexpr std::int64_t kNoPts = -1;

// One complete, framed KLV packet delivered by extraction. `bytes` borrow the
// backend's reassembly buffer and are valid ONLY during the handler call — copy
// to retain (ADR 0011 read-borrows boundary).
struct KlvPacket {
  std::span<const std::byte> bytes;  // parse_packet-able
  std::int64_t pts_ns = kNoPts;      // PES PTS if present, else kNoPts
};

using PacketHandler = std::function<void(const KlvPacket&)>;

struct InsertConfig {
  std::string sink;        // "file:out.ts" | "udp:host:port" | "srt:uri"
  bool realtime = false;   // pace output against the clock (live sinks; B4)
  // v1 signals 0x06 async (0x15 sync deferred, ADR 0008).
};

// Real-time push session (ADR 0013). push() blocks on sink backpressure.
class Inserter {
 public:
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
  virtual Result<std::monostate> extract(std::string_view source,
                                         const PacketHandler& on_packet) = 0;
  virtual Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig&) = 0;
  virtual ~MediaBackend() = default;
};

}  // namespace misbklv
