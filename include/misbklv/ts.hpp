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
// a chunk re-anchors the timeline to that chunk.
//
// PTS wrap limit: PTS is 33 bits and wraps about every 26.5 h. A capture that
// crosses the wrap gets timestamps about 26.5 h off, because the origin is the
// minimum PTS. Workaround: use KLV Item 2 (Precision Time Stamp), or split the
// file before extracting. The live path's behavior across the wrap is not
// specified.
Result<std::monostate> extract_ts_klv(std::span<const std::byte> ts,
                                      const PacketHandler& on_packet);

}  // namespace misbklv
