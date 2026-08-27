// SPDX-License-Identifier: Apache-2.0
// Incremental KLV framing shared by the offline and GStreamer extractors.
// Private implementation detail; not installed.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "misbklv/backend.hpp"
#include "misbklv/packet.hpp"
#include "pts_marks.hpp"

namespace misbklv::detail {

class KlvFramer {
 public:
  explicit KlvFramer(std::size_t max_packet_bytes = kDefaultMaxKlvPacketBytes)
      : max_packet_bytes_(max_packet_bytes) {}

  // Append one timestamped transport unit and emit every complete packet.
  // Returns the first malformed or over-cap frame; an incomplete frame stays
  // buffered for the next feed.
  std::optional<Error> feed(std::span<const std::byte> bytes, std::int64_t pts_ns,
                            const PacketHandler& on_packet) {
    marks_.mark(stream_off_ + reassembly_.size(), pts_ns);
    reassembly_.insert(reassembly_.end(), bytes.begin(), bytes.end());

    std::optional<Error> error;
    std::size_t pos = 0;
    while (pos < reassembly_.size()) {
      auto rest = std::span<const std::byte>(reassembly_).subspan(pos);
      const auto ul =
          std::search(rest.begin(), rest.end(), kSmpteUlPrefix.begin(), kSmpteUlPrefix.end());
      if (ul == rest.end()) {
        // Retain only the longest actual suffix that could complete a UL prefix
        // in the next unit; arbitrary trailing garbage must not accumulate.
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
      rest = std::span<const std::byte>(reassembly_).subspan(pos);
      auto frame = inspect_packet_frame(rest, max_packet_bytes_);
      if (!frame) {
        error = frame.error();
        break;
      }
      if (!*frame) break;
      const std::size_t n = **frame;
      on_packet(KlvPacket{rest.subspan(0, n), marks_.at(stream_off_ + pos)});
      pos += n;
    }
    if (pos) {
      reassembly_.erase(reassembly_.begin(), reassembly_.begin() + pos);
      stream_off_ += pos;
      // A feed that never frames still advances through resynchronized garbage;
      // discard timestamp marks for bytes that can no longer be emitted.
      marks_.prune(stream_off_);
    }
    return error;
  }

  std::span<const std::byte> remainder() const { return reassembly_; }

 private:
  inline static constexpr std::array<std::byte, 4> kSmpteUlPrefix = {
      std::byte{0x06}, std::byte{0x0e}, std::byte{0x2b}, std::byte{0x34}};

  std::size_t max_packet_bytes_;
  std::vector<std::byte> reassembly_;
  PtsMarks marks_;
  std::size_t stream_off_ = 0;
};

}  // namespace misbklv::detail
