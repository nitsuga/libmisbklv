// SPDX-License-Identifier: Apache-2.0
// PtsMarks — attribute a container timestamp to each KLV packet framed out of a
// reassembly buffer (ADR 0021). Private to the implementation: both extractors
// need it, neither exposes it, and it is not part of the installed headers.
//
// The problem it solves: a KLV packet does not line up with the unit that
// carried it. One PES (or one appsink buffer) may hold several packets, and one
// packet may begin in one unit and end in the next. The timestamp that belongs
// to a packet is the one of the unit its FIRST byte fell in — so the extractor
// records a mark per timestamped unit against the absolute offset in the KLV
// byte stream where that unit's bytes begin, and asks for the mark in effect at
// each packet's start.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

#include "misbklv/backend.hpp"  // kNoPts

namespace misbklv::detail {

class PtsMarks {
 public:
  // The bytes appended at absolute stream offset `off` carry `pts` nanoseconds
  // (kNoPts if the container gave that unit no timestamp). Offsets must be
  // non-decreasing.
  void mark(std::size_t off, std::int64_t pts) { marks_.push_back({off, pts}); }

  // The timestamp in effect at absolute offset `off`. Consumes marks, so calls
  // must also be non-decreasing in `off` — which is how both extractors emit.
  std::int64_t at(std::size_t off) {
    while (!marks_.empty() && marks_.front().off <= off) {
      cur_ = marks_.front().pts;
      marks_.pop_front();
    }
    return cur_;
  }

 private:
  struct Mark {
    std::size_t off;
    std::int64_t pts;
  };
  std::deque<Mark> marks_;
  std::int64_t cur_ = kNoPts;  // before the first mark: nothing known
};

}  // namespace misbklv::detail
