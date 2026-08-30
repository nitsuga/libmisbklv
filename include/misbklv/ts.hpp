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
// PES_packet_length ceiling) or split by a muxer is extracted whole. RP 217
// (0x15) metadata AU cells are supported in the non-fragmented form (the common
// case).
//
// Each packet carries `pts_ns` — nanoseconds from the start of the source,
// measured from the earliest PTS anywhere in `ts` (ADR 0021), or `kNoPts` if
// its PES was untimed. `ts` must therefore be the whole stream: extracting from
// a chunk re-anchors the timeline to that chunk.
Result<std::monostate> extract_ts_klv(std::span<const std::byte> ts,
                                      const PacketHandler& on_packet);

}  // namespace misbklv
