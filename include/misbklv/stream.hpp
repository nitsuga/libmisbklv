// SPDX-License-Identifier: Apache-2.0
// KlvStream / KlvSink — the streaming half of the high-level facade (ADR 0018).
// KlvStream adapts the backend's blocking push-extract into a pull range-for over
// owned Messages (background thread + bounded queue). KlvSink emits edited
// Messages to a sink. These need a MediaBackend, so they live in misbklv-gst;
// this header stays gstreamer-free.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "misbklv/backend.hpp"
#include "misbklv/message.hpp"

namespace misbklv {

// Read a KLV source (file path / "udp:host:port" / "srt:uri") as a range of owned
// Messages: `for (Message& m : KlvStream(src)) { ... }`. Iteration ends when the
// source reaches EOS (file) or idles (live). NB: breaking early blocks the
// destructor until the source drains — clean for files; for an endless live
// source it waits one idle timeout (a cooperative stop is an ADR 0017 follow-on).
class KlvStream {
 public:
  explicit KlvStream(std::string source);  // default gstreamer backend
  KlvStream(std::unique_ptr<MediaBackend> backend, std::string source);
  ~KlvStream();

  KlvStream(const KlvStream&) = delete;
  KlvStream& operator=(const KlvStream&) = delete;

  struct Sentinel {};
  class Iterator {
   public:
    explicit Iterator(KlvStream* s) : s_(s) {}
    Message& operator*() { return *s_->current_; }
    Iterator& operator++() { s_->pull(); return *this; }
    bool operator!=(Sentinel) const { return s_->current_.has_value(); }

   private:
    KlvStream* s_;
  };
  Iterator begin() { pull(); return Iterator(this); }
  Sentinel end() { return {}; }

 private:
  struct Frame { std::vector<std::byte> bytes; std::int64_t pts; };
  void pull();  // pop next frame -> parse into current_, or reset at end

  std::unique_ptr<MediaBackend> backend_;
  std::string source_;
  std::optional<Message> current_;

  // Bounded producer/consumer queue between the extract thread and the iterator.
  std::mutex mu_;
  std::condition_variable not_full_, not_empty_;
  std::deque<Frame> queue_;
  static constexpr std::size_t kCap = 32;
  bool done_ = false;   // extract returned
  bool stop_ = false;   // destructor asked the producer to quit
  std::thread producer_;

  void push_frame(Frame f);
  bool pop_frame(Frame& out);
};

// Emit (edited) Messages to a sink ("file:out.ts" / "udp:host:port" / "srt:uri").
// realtime=true paces output on the clock (ADR 0017).
class KlvSink {
 public:
  explicit KlvSink(std::string sink, bool realtime = false);
  KlvSink(std::unique_ptr<MediaBackend> backend, std::string sink,
          bool realtime = false);

  Result<std::monostate> emit(const Message& m);  // m.encode() -> push
  Result<std::monostate> close();                 // drain + finish

 private:
  std::unique_ptr<MediaBackend> backend_;
  std::unique_ptr<Inserter> inserter_;
};

}  // namespace misbklv
