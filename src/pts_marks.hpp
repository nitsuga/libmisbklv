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

  // Drop marks whose offset falls behind `stream_off` — their bytes have
  // already left the caller's reassembly buffer, so no future `at()` call can
  // land on them. Unlike `at()`, this does not require a completed frame: a
  // feed that never frames still advances `stream_off` via resync (dropping
  // unmatched bytes), and without this the queue would grow one entry per
  // input unit forever. `stream_off` itself is still live, so the bound is
  // strict: only entries strictly behind it are dropped.
  void prune(std::size_t stream_off) {
    while (!marks_.empty() && marks_.front().off < stream_off) {
      cur_ = marks_.front().pts;
      marks_.pop_front();
    }
  }

  // Pending mark count. Testing/introspection only — no extraction logic
  // depends on the queue's size, only on at()/prune() keeping it bounded.
  std::size_t pending() const { return marks_.size(); }

 private:
  struct Mark {
    std::size_t off;
    std::int64_t pts;
  };
  std::deque<Mark> marks_;
  std::int64_t cur_ = kNoPts;  // before the first mark: nothing known
};

}  // namespace misbklv::detail
