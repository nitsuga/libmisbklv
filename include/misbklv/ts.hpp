// SPDX-License-Identifier: Apache-2.0
// Minimal gstreamer-free MPEG-TS KLV extractor. Finds the KLV elementary PID by
// content (a PES payload starting with a SMPTE UL) and reassembles KLV packets,
// handling BOTH signaling types: 0x06 (KLV directly in the PES) and 0x15 (KLV in
// SMPTE RP 217 metadata AU cells). The 0x15 case is the one stock gstreamer
// tsdemux drops (fork 12); this covers it with no dependency.
#pragma once

#include <cstddef>
#include <span>

#include "misbklv/backend.hpp"  // PacketHandler, KlvPacket, Result

namespace misbklv {

// Extract KLV packets from a buffer of MPEG-TS bytes; calls `on_packet` per
// framed packet (bytes borrowed during the call, per ADR 0013). Returns a
// Backend error if no KLV PID is found. Non-fragmented AU cells (the common
// case) are supported.
//
// Each packet carries `pts_ns` — nanoseconds from the start of the source,
// measured from the earliest PTS anywhere in `ts` (ADR 0021), or `kNoPts` if
// its PES was untimed. `ts` must therefore be the whole stream: extracting from
// a chunk re-anchors the timeline to that chunk.
Result<std::monostate> extract_ts_klv(std::span<const std::byte> ts,
                                      const PacketHandler& on_packet);

}  // namespace misbklv
