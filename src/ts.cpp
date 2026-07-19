// SPDX-License-Identifier: Apache-2.0
#include "misbklv/ts.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "misbklv/packet.hpp"  // packet_frame_length

namespace misbklv {
namespace {

constexpr std::uint8_t kSmpteUl[4] = {0x06, 0x0e, 0x2b, 0x34};

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

}  // namespace

Result<std::monostate> extract_ts_klv(std::span<const std::byte> ts,
                                      const PacketHandler& on_packet) {
  constexpr std::size_t kPkt = 188;
  std::unordered_map<std::uint16_t, std::vector<std::byte>> pes;  // PID -> current PES
  int klv_pid = -1;
  std::vector<std::byte> reasm;  // reassembled KLV for the KLV PID

  auto flush = [&](std::uint16_t pid) {
    auto it = pes.find(pid);
    if (it == pes.end() || it->second.empty()) return;
    const auto klv = unwrap_pes(it->second);
    if (starts_with_ul(klv)) {
      if (klv_pid < 0) klv_pid = pid;
      if (pid == static_cast<std::uint16_t>(klv_pid)) {
        reasm.insert(reasm.end(), klv.begin(), klv.end());
        std::size_t pos = 0;
        for (;;) {
          std::span<const std::byte> rest(reasm.data() + pos, reasm.size() - pos);
          const std::size_t n = packet_frame_length(rest);
          if (n == 0) break;
          on_packet(KlvPacket{rest.subspan(0, n), kNoPts});
          pos += n;
        }
        if (pos) reasm.erase(reasm.begin(), reasm.begin() + pos);
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
      pes[pid].assign(payload.begin(), payload.end());
    } else {
      auto it = pes.find(pid);
      if (it != pes.end() && !it->second.empty())
        it->second.insert(it->second.end(), payload.begin(), payload.end());
    }
  }
  for (auto& [pid, _] : pes) flush(pid);
  return klv_pid >= 0 ? Result<std::monostate>::ok({})
                      : Result<std::monostate>::err(Error::Backend);
}

}  // namespace misbklv
