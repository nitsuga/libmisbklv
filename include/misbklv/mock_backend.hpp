// SPDX-License-Identifier: Apache-2.0
// In-memory MediaBackend for testing the ADR 0013 contract without gstreamer.
#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "misbklv/backend.hpp"
#include "misbklv/ber.hpp"

namespace misbklv {

// Captures pushed packets so a test can assert on them.
class MockInserter : public Inserter {
 public:
  std::vector<ber::Bytes> pushed;
  std::vector<std::int64_t> pts;
  bool finished = false;

  Result<std::monostate> push(std::span<const std::byte> p, std::int64_t t) override {
    pushed.emplace_back(p.begin(), p.end());
    pts.push_back(t);
    return Result<std::monostate>::ok({});
  }
  Result<std::monostate> finish() override {
    finished = true;
    return Result<std::monostate>::ok({});
  }
};

// extract() replays canned packets (bytes borrowed from the backend, per ADR
// 0013); open_insert() returns a MockInserter (also kept in `last_inserter`).
class MockBackend : public MediaBackend {
 public:
  // `pts` (optional, parallel to `packets`) are the timestamps extract() reports
  // — nanoseconds from the start of the source, as a real backend does (ADR
  // 0021). Omit it to replay an untimed stream: every packet gets kNoPts, which
  // is what a capture like Day Flight actually yields.
  explicit MockBackend(std::vector<ber::Bytes> packets = {},
                       std::vector<std::int64_t> pts = {})
      : packets_(std::move(packets)), pts_(std::move(pts)) {}

  Result<std::monostate> extract(std::string_view, const PacketHandler& on_packet,
                                 std::stop_token stop = {}) override {
    for (std::size_t i = 0; i < packets_.size(); ++i) {
      if (stop.stop_requested()) break;  // cooperative cancel (ADR 0019)
      on_packet(KlvPacket{std::span<const std::byte>(packets_[i]),
                          i < pts_.size() ? pts_[i] : kNoPts});
    }
    return Result<std::monostate>::ok({});
  }

  Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig&) override {
    auto ins = std::make_unique<MockInserter>();
    last_inserter = ins.get();  // non-owning, for test inspection
    return Result<std::unique_ptr<Inserter>>::ok(std::move(ins));
  }

  MockInserter* last_inserter = nullptr;

 private:
  std::vector<ber::Bytes> packets_;
  std::vector<std::int64_t> pts_;  // short/empty -> kNoPts from there on
};

}  // namespace misbklv
