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
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "misbklv/backend.hpp"
#include "misbklv/message.hpp"

namespace misbklv {

// Read a KLV source (file path / "udp:host:port" / "srt:uri") as a range of owned
// Messages: `for (Message& m : KlvStream(src)) { ... }`. Each Message carries the
// source's timing in `pts()` — nanoseconds from the start of the source, the same
// timeline KlvSink emits on, so `KlvStream -> edit -> KlvSink` keeps a stream
// where it was (ADR 0021). `kNoPts` for a source whose KLV PES are untimed.
// Iteration ends when a file reaches EOS or UDP idles after data; SRT has no
// natural idle end. Breaking early is safe: the destructor cooperatively cancels
// the extract (ADR 0019), so it returns promptly even for an endless live source
// (no wait for the idle timeout / EOS).
class KlvStream {
 public:
  explicit KlvStream(std::string source, ExtractOptions options = {});
  KlvStream(std::unique_ptr<MediaBackend> backend, std::string source,
            ExtractOptions options = {});
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

  // Terminal extraction or Message parse error, for inspection after iteration.
  // EOS and cooperative cancellation leave this empty.
  std::optional<Error> error() const;

 private:
  struct Frame { std::vector<std::byte> bytes; std::int64_t pts; };
  void pull();  // pop next frame -> parse into current_, or reset at end

  std::unique_ptr<MediaBackend> backend_;
  std::string source_;
  ExtractOptions options_;
  std::optional<Message> current_;

  // Bounded producer/consumer queue between the extract thread and the iterator.
  mutable std::mutex mu_;
  std::condition_variable not_full_, not_empty_;
  std::deque<Frame> queue_;
  static constexpr std::size_t kCap = 32;
  bool done_ = false;   // extract returned
  bool stop_ = false;   // destructor or a terminal parse error stopped the producer
  std::optional<Error> error_;          // terminal error exposed after iteration
  std::optional<Error> backend_error_;  // pending until queued frames drain
  std::stop_source stop_source_;  // cancels the backend extract (ADR 0019)
  std::thread producer_;

  void push_frame(Frame f);
  bool pop_frame(Frame& out);
};

// Emit (edited) Messages to a sink ("file:out.ts" / "udp:host:port" / "srt:uri").
// realtime=true paces output on the clock (ADR 0017).
//
// For a `file:` sink, an output file exists only if `close()` returned ok: a
// failing close, or destroying the sink without one, removes the file this
// session created (never one that was already there — ADR 0022).
//
// `video_source` accepts a path / "file:PATH", an RTSP URI, or a
// "pipeline:<gst-launch desc>" live source and re-muxes its video elementary
// stream alongside the KLV — see InsertConfig for the full contract. With it
// set, every emitted Message must carry a real PTS on the source timeline
// (`Message::set_pts`; file-relative or live running time, in nanoseconds); an
// unset PTS is rejected rather than synthesized. `realtime` is supported with
// both file and live video (ADR 0031).
class KlvSink {
 public:
  explicit KlvSink(std::string sink, bool realtime = false,
                   std::string video_source = {},
                   Sei0604 sei_0604 = Sei0604::Preserve);
  KlvSink(std::unique_ptr<MediaBackend> backend, std::string sink,
          bool realtime = false, std::string video_source = {},
          Sei0604 sei_0604 = Sei0604::Preserve);

  // Same, taking the config whole — clearer than four positional arguments once
  // more than the sink is set.
  explicit KlvSink(InsertConfig cfg);
  KlvSink(std::unique_ptr<MediaBackend> backend, InsertConfig cfg);

  Result<std::monostate> emit(const Message& m);  // m.encode() -> push
  // drain + finish. `stop` cancels the post-EOS drain early — a realtime file
  // replay otherwise drains its video at wall-clock speed and ignores a Ctrl-C
  // that lands after the KLV is emitted (ADR 0032). Default token never signals.
  Result<std::monostate> close(std::stop_token stop = {});

  // Failure from open_insert(), if any. Runtime emit()/close() failures remain
  // reported by their Result values and do not change this accessor.
  std::optional<Error> error() const { return open_error_; }

 private:
  std::unique_ptr<MediaBackend> backend_;
  std::unique_ptr<Inserter> inserter_;
  std::optional<Error> open_error_;
};

}  // namespace misbklv
