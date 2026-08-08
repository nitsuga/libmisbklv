// SPDX-License-Identifier: Apache-2.0
// Cooperative extract cancellation (ADR 0019). An endless live source is
// simulated by a mock that emits until its std::stop_token is signaled. If the
// token were ignored, both tests would hang forever (CI timeout) — so reaching
// the end at all proves early exit works; the timing bound proves it's prompt.
// argv: <input.klv>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "misbklv/backend.hpp"
#include "misbklv/mock_backend.hpp"
#include "misbklv/stream.hpp"

using namespace misbklv;
using clock_t_ = std::chrono::steady_clock;

static int failures = 0;
static void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}
static std::vector<std::byte> read_file(const char* p) {
  std::ifstream f(p, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i)
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}

// An endless live source: re-emits one packet every ~2 ms until cancelled.
class StreamingMock : public MediaBackend {
 public:
  explicit StreamingMock(std::vector<std::byte> pkt) : pkt_(std::move(pkt)) {}
  std::atomic<int> emitted{0};

  Result<std::monostate> extract(std::string_view, const PacketHandler& on_packet,
                                 std::stop_token stop = {},
                                 ExtractOptions = {}) override {
    while (!stop.stop_requested()) {
      on_packet(KlvPacket{std::span<const std::byte>(pkt_), kNoPts});
      emitted.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return Result<std::monostate>::ok({});
  }
  Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig&) override {
    return Result<std::unique_ptr<Inserter>>::err(Error::Unsupported);
  }

 private:
  std::vector<std::byte> pkt_;
};

// Delivers its canned frames, then returns one chosen terminal extraction error.
class FailingBackend : public MediaBackend {
 public:
  FailingBackend(std::vector<ber::Bytes> packets, Error error)
      : packets_(std::move(packets)), error_(error) {}

  Result<std::monostate> extract(std::string_view, const PacketHandler& on_packet,
                                 std::stop_token stop = {}, ExtractOptions = {}) override {
    for (const auto& packet : packets_) {
      if (stop.stop_requested()) return Result<std::monostate>::ok({});
      on_packet(KlvPacket{packet, kNoPts});
    }
    return Result<std::monostate>::err(error_);
  }
  Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig&) override {
    return Result<std::unique_ptr<Inserter>>::err(Error::Unsupported);
  }

 private:
  std::vector<ber::Bytes> packets_;
  Error error_;
};

class UnsupportedInsertBackend : public MediaBackend {
 public:
  Result<std::monostate> extract(std::string_view, const PacketHandler&,
                                 std::stop_token = {}, ExtractOptions = {}) override {
    return Result<std::monostate>::ok({});
  }
  Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig&) override {
    return Result<std::unique_ptr<Inserter>>::err(Error::Unsupported);
  }
};

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: stop_test <input.klv>\n");
    return 2;
  }
  const char* path = argv[1];
  const auto pkt = read_file(path);
  if (pkt.empty()) { std::fprintf(stderr, "no fixture: %s\n", path); return 2; }

  // --- (1) KlvStream: break early out of the range-for over an endless source -
  std::printf("(1) KlvStream early break\n");
  int read = 0;
  const auto t0 = clock_t_::now();
  {
    KlvStream stream(std::make_unique<StreamingMock>(pkt), "endless");
    for (Message& m : stream) {
      (void)m;
      if (++read >= 3) break;  // leaving the loop destroys `stream`
    }
    check(!stream.error(), "early break has no spurious stream error while alive");
  }  // ~KlvStream must cancel the extract and return promptly (not hang)
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      clock_t_::now() - t0).count();
  check(read == 3, "read exactly 3 messages then broke");
  check(ms < 2000, "break + destroy completed promptly (no wait for stream end)");
  std::printf("    (destroyed in %lld ms)\n", static_cast<long long>(ms));

  // --- (2) direct MediaBackend::extract cancellation --------------------------
  std::printf("(2) extract(stop_token) cancellation\n");
  StreamingMock mock(pkt);
  std::stop_source src;
  std::thread th([&] { mock.extract("x", [](const KlvPacket&) {}, src.get_token()); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  check(mock.emitted.load() >= 1, "extract emits while running");
  src.request_stop();
  th.join();  // reaching past join proves extract returned after the request
  const int after = mock.emitted.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  check(mock.emitted.load() == after, "no emissions after stop");

  // --- (3) facade observes normal EOS and exact terminal errors -------------
  std::printf("(3) KlvStream terminal errors\n");
  {
    KlvStream stream(std::make_unique<MockBackend>(std::vector<ber::Bytes>{pkt}), "mock");
    int messages = 0;
    for (Message& m : stream) {
      (void)m;
      ++messages;
    }
    check(messages == 1 && !stream.error(), "normal MockBackend EOS yields message and no error");
  }
  {
    KlvStream stream(std::make_unique<FailingBackend>(std::vector<ber::Bytes>{pkt},
                                                       Error::ResourceLimit),
                     "failing");
    int messages = 0;
    for (Message& m : stream) {
      (void)m;
      ++messages;
    }
    check(messages == 1 && stream.error() == Error::ResourceLimit,
          "queued valid message precedes exact backend ResourceLimit");
  }
  {
    KlvStream stream(std::make_unique<MockBackend>(std::vector<ber::Bytes>{{}}), "malformed");
    int messages = 0;
    for (Message& m : stream) {
      (void)m;
      ++messages;
    }
    check(messages == 0 && stream.error() == Error::Truncated,
          "malformed MockBackend packet terminates with Message parse error");
  }
  {
    ber::Bytes unknown;
    for (std::uint8_t b : kUas0601Key) unknown.push_back(static_cast<std::byte>(b));
    unknown[4] = std::byte{0xFF};  // structurally packet-like, but unregistered UL
    unknown.push_back(std::byte{0x00});
    KlvStream stream(std::make_unique<MockBackend>(std::vector<ber::Bytes>{unknown}), "unknown");
    int messages = 0;
    for (Message& m : stream) {
      (void)m;
      ++messages;
    }
    check(messages == 0 && stream.error() == Error::UnknownTag,
          "unknown MockBackend UL is not silently skipped");
  }
  {
    KlvStream stream(std::make_unique<MockBackend>(std::vector<ber::Bytes>{pkt}), "limited",
                     ExtractOptions{.max_packet_bytes = pkt.size() - 1});
    int messages = 0;
    for (Message& m : stream) {
      (void)m;
      ++messages;
    }
    check(messages == 0 && stream.error() == Error::ResourceLimit,
          "KlvStream forwards ExtractOptions to backend");
  }

  // The default GStreamer facade reports an unreadable source as Backend rather
  // than producing an empty, apparently-clean stream.
  {
    KlvStream stream("file:/tmp/misbklv-definitely-missing-input.ts");
    int messages = 0;
    for (Message& m : stream) {
      (void)m;
      ++messages;
    }
    check(messages == 0 && stream.error() == Error::Backend,
          "missing GStreamer source yields Backend with zero messages");
  }

  // An open_insert failure stays observable on the facade and is returned by
  // both later operations, instead of degrading to the generic Backend error.
  {
    auto fresh = Message::create(RegistryId::Uas0601);
    KlvSink sink(std::make_unique<UnsupportedInsertBackend>(), "unsupported");
    const auto emitted = fresh ? sink.emit(*fresh)
                               : Result<std::monostate>::err(Error::Backend);
    const auto closed = sink.close();
    check(sink.error() == Error::Unsupported && !emitted &&
              emitted.error() == Error::Unsupported && !closed &&
              closed.error() == Error::Unsupported,
          "KlvSink exposes open_insert Unsupported through error/emit/close");
  }

  std::printf("%s\n", failures == 0 ? "STOP: all PASS" : "STOP: FAIL");
  return failures == 0 ? 0 : 1;
}
