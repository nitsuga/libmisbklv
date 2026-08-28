// SPDX-License-Identifier: Apache-2.0
#include "misbklv/stream.hpp"

#include <utility>

#include "misbklv/gst_backend.hpp"

namespace misbklv {

// --- KlvStream --------------------------------------------------------------
KlvStream::KlvStream(std::unique_ptr<MediaBackend> backend, std::string source,
                     ExtractOptions options)
    : backend_(std::move(backend)), source_(std::move(source)), options_(options) {
  producer_ = std::thread([this] {
    auto result = backend_->extract(
        source_,
        [this](const KlvPacket& kp) {
          push_frame(Frame{std::vector<std::byte>(kp.bytes.begin(), kp.bytes.end()), kp.pts_ns});
        },
        stop_source_.get_token(), options_);
    {  // extract returned -> no more frames
      std::lock_guard<std::mutex> lk(mu_);
      if (!stop_ && !stop_source_.stop_requested() && !result) backend_error_ = result.error();
      done_ = true;
    }
    not_empty_.notify_all();
  });
}

KlvStream::KlvStream(std::string source, ExtractOptions options)
    : KlvStream(make_gst_backend(), std::move(source), options) {}

KlvStream::~KlvStream() {
  {  // unblock a push_frame waiting on a full queue, so extract's teardown
     // (set_state NULL, which waits on the streaming thread) can't deadlock
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  not_full_.notify_all();
  not_empty_.notify_all();
  stop_source_.request_stop();  // cancel the extract itself (ADR 0019) — the
                                // only way to end an endless live source early
  if (producer_.joinable()) producer_.join();
}

void KlvStream::push_frame(Frame f) {
  std::unique_lock<std::mutex> lk(mu_);
  not_full_.wait(lk, [this] { return queue_.size() < kCap || stop_; });
  if (stop_) return;  // consumer gone; drop (extract will run to completion)
  queue_.push_back(std::move(f));
  not_empty_.notify_one();
}

bool KlvStream::pop_frame(Frame& out) {
  std::unique_lock<std::mutex> lk(mu_);
  not_empty_.wait(lk, [this] { return !queue_.empty() || done_ || stop_; });
  if (queue_.empty()) {
    if (done_ && !stop_ && !error_ && backend_error_) error_ = backend_error_;
    return false;  // done/stop and drained
  }
  out = std::move(queue_.front());
  queue_.pop_front();
  not_full_.notify_one();
  return true;
}

void KlvStream::pull() {
  for (;;) {
    Frame f;
    if (!pop_frame(f)) {  // end of stream
      current_.reset();
      return;
    }
    auto m = Message::adopt(std::move(f.bytes));
    if (m) {
      m->set_pts(f.pts);
      current_.emplace(std::move(*m));
      return;
    }
    {  // A malformed/unrecognized packet is terminal at this stream position.
      std::lock_guard<std::mutex> lk(mu_);
      error_ = m.error();
      queue_.clear();
      stop_ = true;
    }
    stop_source_.request_stop();
    not_full_.notify_all();
    not_empty_.notify_all();
    current_.reset();
    return;
  }
}

std::optional<Error> KlvStream::error() const {
  std::lock_guard<std::mutex> lk(mu_);
  return error_;
}

// --- KlvSink ----------------------------------------------------------------
KlvSink::KlvSink(std::unique_ptr<MediaBackend> backend, InsertConfig cfg)
    : backend_(std::move(backend)) {
  auto ins = backend_->open_insert(std::move(cfg));
  if (ins)
    inserter_ = std::move(*ins);
  else
    open_error_ = ins.error();
}

KlvSink::KlvSink(InsertConfig cfg) : KlvSink(make_gst_backend(), std::move(cfg)) {}

KlvSink::KlvSink(std::unique_ptr<MediaBackend> backend, std::string sink, bool realtime,
                 std::string video_source, Sei0604 sei_0604)
    : KlvSink(std::move(backend),
              InsertConfig{std::move(sink), realtime, std::move(video_source), sei_0604}) {}

KlvSink::KlvSink(std::string sink, bool realtime, std::string video_source, Sei0604 sei_0604)
    : KlvSink(make_gst_backend(), std::move(sink), realtime, std::move(video_source), sei_0604) {}

Result<std::monostate> KlvSink::emit(const Message& m) {
  if (!inserter_) return Result<std::monostate>::err(open_error_.value_or(Error::Backend));
  if (!m.edited()) {
    auto orig = m.original_bytes();
    if (!orig.empty()) return inserter_->push(orig, m.pts());
  }
  auto bytes = m.encode();
  if (!bytes) return Result<std::monostate>::err(bytes.error());
  return inserter_->push(std::move(*bytes), m.pts());
}

Result<std::monostate> KlvSink::close(std::stop_token stop) {
  if (!inserter_) return Result<std::monostate>::err(open_error_.value_or(Error::Backend));
  return inserter_->finish(std::move(stop));
}

Result<bool> KlvSink::poll() {
  if (!inserter_) return Result<bool>::err(open_error_.value_or(Error::Backend));
  return inserter_->poll();
}

}  // namespace misbklv
