// SPDX-License-Identifier: Apache-2.0
// Cooperative extract cancellation (ADR 0019). An endless live source is
// simulated by a mock that emits until its std::stop_token is signaled. If the
// token were ignored, both tests would hang forever (CI timeout) — so reaching
// the end at all proves early exit works; the timing bound proves it's prompt.
// argv: <dayflight_first_packet.klv>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "misbklv/backend.hpp"
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
                                 std::stop_token stop = {}) override {
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

int main(int argc, char** argv) {
  const char* path =
      argc > 1 ? argv[1] : "test/fixtures/dayflight_first_packet.klv";
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

  std::printf("%s\n", failures == 0 ? "STOP: all PASS" : "STOP: FAIL");
  return failures == 0 ? 0 : 1;
}
