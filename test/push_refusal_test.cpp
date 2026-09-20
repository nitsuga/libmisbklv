// SPDX-License-Identifier: Apache-2.0
// Appsrc refusal before the first finish() (issue #83): the first refused push
// latches a terminal Backend error, later pushes fail fast without touching the
// appsrc, and finish() discards the output file this session created.
// argv: <input.klv> <temp.ts>  (temp.ts must not exist)
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gst_backend_internal.hpp"
#include "misbklv/gst_backend.hpp"
#include "misbklv/packet.hpp"

using namespace misbklv;

static int g_hook_calls = 0;
static GstFlowReturn refuse(GstAppSrc*, GstBuffer* buf) {
  ++g_hook_calls;
  gst_buffer_unref(buf);  // the real push owns the buffer even on failure
  return GST_FLOW_FLUSHING;
}

static bool check(bool ok, const char* what) {
  if (!ok) std::fprintf(stderr, "FAIL: %s\n", what);
  return ok;
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  std::ifstream f(argv[1], std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<std::byte> in(reinterpret_cast<const std::byte*>(raw.data()),
                            reinterpret_cast<const std::byte*>(raw.data()) + raw.size());
  const std::size_t n = packet_frame_length(in);
  if (n == 0) return 2;
  const std::span<const std::byte> pkt = std::span(in).first(n);
  const std::filesystem::path out = argv[2];
  std::filesystem::remove(out);

  auto be = make_gst_backend();
  auto ins = be->open_insert({"file:" + out.string(), false, "", Sei0604::Preserve});
  if (!ins) return 2;
  auto& hook = detail::appsrc_push_hook();
  const auto real = hook;

  bool ok = check(static_cast<bool>((*ins)->push(pkt, kNoPts)), "first push with real hook");
  ok = ok && check(std::filesystem::exists(out), "output created before refusal");

  hook = &refuse;
  const auto r1 = (*ins)->push(pkt, kNoPts);
  const int calls_after_first = g_hook_calls;
  const auto r2 = (*ins)->push(pkt, kNoPts);
  const auto polled = (*ins)->poll();
  const auto fin = (*ins)->finish();
  const auto fin2 = (*ins)->finish();
  hook = real;

  ok = ok && check(!r1 && r1.error() == Error::Backend, "refused push -> Backend");
  ok = ok && check(calls_after_first == 1, "hook called once");
  ok = ok && check(!r2 && r2.error() == Error::Backend && g_hook_calls == 1,
                   "later push fails fast without the hook");
  ok = ok && check(!polled && polled.error() == Error::Backend, "poll -> Backend");
  ok = ok && check(!fin && fin.error() == Error::Backend, "finish -> Backend");
  ok = ok && check(!std::filesystem::exists(out), "output discarded");
  ok = ok && check(!fin2 && fin2.error() == Error::Backend, "second finish cached error");
  std::filesystem::remove(out);
  return ok ? 0 : 1;
}
