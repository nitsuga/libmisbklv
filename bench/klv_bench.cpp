// SPDX-License-Identifier: Apache-2.0
// Timing harness for the paths issue #50 parks work behind. Not a test: it
// asserts nothing and is not registered with CTest, because a timing threshold
// in CI measures the runner more than the code. It is built by default so it
// cannot bit-rot, and run by hand when a number is needed.
//
// Reports the median of several reps rather than the mean: a background process
// on the machine skews the mean and leaves the median alone.
//
// argv: <fixture.klv> <fixture.ts> [target_MiB]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "misbklv/codec.hpp"
#include "misbklv/message.hpp"
#include "misbklv/registries.hpp"
#include "misbklv/ts.hpp"

#ifdef MISBKLV_BENCH_GST
#include "misbklv/gst_backend.hpp"
#endif

using namespace misbklv;

namespace {

// Kept live so the optimizer cannot delete the work being timed.
volatile std::uint64_t g_sink = 0;

std::vector<std::byte> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i)
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}

// Median nanoseconds per iteration over `reps` runs of `iters` iterations.
template <class F>
double median_ns_per_iter(int reps, std::size_t iters, F&& body) {
  // Warm up before the clock starts. Without this the first row measured reads
  // high on an idle machine — that is CPU frequency ramp-up and cold caches,
  // not the code under test.
  for (std::size_t i = 0; i < std::min<std::size_t>(iters, 1000); ++i) body();
  std::vector<double> per_iter;
  per_iter.reserve(reps);
  for (int r = 0; r < reps; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iters; ++i) body();
    const auto t1 = std::chrono::steady_clock::now();
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    per_iter.push_back(static_cast<double>(ns) / static_cast<double>(iters));
  }
  std::sort(per_iter.begin(), per_iter.end());
  return per_iter[per_iter.size() / 2];
}

void report(const char* what, double ns) {
  std::printf("  %-42s %10.1f ns/op\n", what, ns);
}

void report_throughput(const char* what, double ns, std::size_t bytes) {
  const double mib_per_s = (static_cast<double>(bytes) / (1024.0 * 1024.0)) /
                           (ns / 1e9);
  std::printf("  %-42s %10.1f ns/op  %8.1f MiB/s\n", what, ns, mib_per_s);
}

// First item of `kind` whose encode width the numeric codecs accept. `variable`
// is deliberately not filtered on: every IMAPB item in the 0601 registry is
// variable-width with a `fixed_len` default, so excluding them would drop the
// one kind this benchmark exists to measure. `fixed_len` is the encode width in
// both cases (see ItemDescriptor). Tag 1 is skipped — it is the checksum, which
// Message::set() rejects as read-only, and the edit row below reuses this.
const ItemDescriptor* first_of_kind(const Registry& reg, ValueKind kind) {
  for (const auto& d : reg.items)
    if (d.kind == kind && d.tag != 1 && d.fixed_len >= 1 && d.fixed_len <= 8)
      return &d;
  return nullptr;
}

// --- (1) per-kind codec round trip ------------------------------------------
// The measurement issue #49 is gated on: IMAPB recomputes ceil/log2/exp2 per
// item, UInt does not, so the gap between these two rows is the cost that
// precomputing the scaling factors would remove.
void bench_codec(const Registry& reg) {
  std::printf("codec decode+encode, by value kind (one item)\n");
  const ValueKind kinds[] = {ValueKind::UInt, ValueKind::Int,
                             ValueKind::LinearLDS, ValueKind::IMAPB};
  const char* names[] = {"UInt", "Int", "LinearLDS", "IMAPB"};
  for (std::size_t k = 0; k < std::size(kinds); ++k) {
    const ItemDescriptor* d = first_of_kind(reg, kinds[k]);
    if (!d) {
      std::printf("  %-42s (no fixed-width item in registry)\n", names[k]);
      continue;
    }
    // A mid-range payload: the top two bits stay clear so an IMAPB item is a
    // normal mapped value rather than a structural special (ST 1201 7.2.3),
    // which would short-circuit the very math being measured.
    std::vector<std::byte> raw(d->fixed_len, std::byte{0x11});
    raw[0] = std::byte{0x40};

    char label[96];
    std::snprintf(label, sizeof(label), "%s (tag %u, %u bytes)", names[k],
                  static_cast<unsigned>(d->tag),
                  static_cast<unsigned>(d->fixed_len));
    const double ns = median_ns_per_iter(5, 200000, [&] {
      auto v = codec::decode(*d, raw);
      if (!v) return;
      auto enc = codec::encode(*d, *v, d->fixed_len);
      if (enc) g_sink += enc->size();
    });
    report(label, ns);
  }
}

// --- (2) whole-message round trip -------------------------------------------
void bench_message(std::span<const std::byte> klv) {
  std::printf("message round trip (%zu-byte packet)\n", klv.size());
  const double parse_ns = median_ns_per_iter(5, 20000, [&] {
    auto m = Message::parse(klv);
    if (m) g_sink += m->items().size();
  });
  report_throughput("Message::parse", parse_ns, klv.size());

  auto parsed = Message::parse(klv);
  if (!parsed) {
    std::printf("  (fixture did not parse; skipping encode)\n");
    return;
  }
  // Unedited encode returns the source bytes verbatim, so this row is the copy
  // path, not the builder. Both are worth knowing apart.
  const double encode_ns = median_ns_per_iter(5, 20000, [&] {
    auto out = parsed->encode();
    if (out) g_sink += out->size();
  });
  report_throughput("Message::encode (unedited passthrough)", encode_ns,
                    klv.size());

  auto edited = Message::parse(klv);
  const ItemDescriptor* uint_item =
      first_of_kind(*registry_for(RegistryId::Uas0601), ValueKind::UInt);
  if (!edited || !uint_item) {
    std::printf("  (no editable UInt item; skipping the rebuild row)\n");
    return;
  }
  auto set_ok = edited->set(uint_item->tag, Value{std::uint64_t{1}});
  if (!set_ok) {
    std::printf("  (set(tag %u) failed with error %d; skipping rebuild row)\n",
                static_cast<unsigned>(uint_item->tag),
                static_cast<int>(set_ok.error()));
    return;
  }
  const double reencode_ns = median_ns_per_iter(5, 20000, [&] {
    auto out = edited->encode();
    if (out) g_sink += out->size();
  });
  report_throughput("Message::encode (one edit, rebuilds)", reencode_ns,
                    klv.size());
}

// --- (3) gst-free TS extraction ---------------------------------------------
// Answers the second half of #50: extract_ts_klv walks the buffer twice, once
// in earliest_pts_90k and once in the main loop. The MiB/s here is what that
// costs end to end; halving it is the ceiling any single-pass redesign could
// reach.
void bench_ts_extract(const std::vector<std::byte>& ts_repeated) {
  std::printf("extract_ts_klv over %.1f MiB of MPEG-TS\n",
              static_cast<double>(ts_repeated.size()) / (1024.0 * 1024.0));
  std::size_t packets = 0;
  const double ns = median_ns_per_iter(5, 1, [&] {
    packets = 0;
    auto r = extract_ts_klv(ts_repeated, [&](const KlvPacket& kp) {
      ++packets;
      g_sink += kp.bytes.size();
    });
    if (!r) g_sink += 1;
  });
  report_throughput("extract_ts_klv (2 passes: origin + frame)", ns,
                    ts_repeated.size());
  std::printf("  (%zu packets extracted)\n", packets);
}

#ifdef MISBKLV_BENCH_GST
// --- (4) gst extraction over the same bytes ---------------------------------
// The comparison #50 asks for. This includes gstreamer pipeline setup and
// teardown, so it is not a like-for-like against (3) on small inputs — the
// larger the stream, the more of this row is steady-state demux.
void bench_gst_extract(const std::vector<std::byte>& ts_repeated) {
  const auto tmp = std::filesystem::temp_directory_path() /
                   "misbklv_bench_input.ts";
  {
    std::ofstream f(tmp, std::ios::binary);
    f.write(reinterpret_cast<const char*>(ts_repeated.data()),
            static_cast<std::streamsize>(ts_repeated.size()));
  }
  std::printf("gst extract over the same %.1f MiB (pipeline setup included)\n",
              static_cast<double>(ts_repeated.size()) / (1024.0 * 1024.0));
  std::size_t packets = 0;
  const double ns = median_ns_per_iter(3, 1, [&] {
    packets = 0;
    auto backend = make_gst_backend();
    auto r = backend->extract(
        "file:" + tmp.string(),
        [&](const KlvPacket& kp) {
          ++packets;
          g_sink += kp.bytes.size();
        });
    if (!r) g_sink += 1;
  });
  report_throughput("GstBackend::extract", ns, ts_repeated.size());
  std::printf("  (%zu packets extracted)\n", packets);
  std::error_code ec;
  std::filesystem::remove(tmp, ec);
}
#endif

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: klv_bench <fixture.klv> <fixture.ts> [MiB]\n");
    return 2;
  }
  const auto klv = read_file(argv[1]);
  const auto ts = read_file(argv[2]);
  if (klv.empty() || ts.empty()) {
    std::fprintf(stderr, "could not read fixtures\n");
    return 2;
  }
  const std::size_t target_mib = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 8;

  // The .ts fixtures are ~1.5 KiB, far too small to say anything about a
  // memory-bound sweep. Concatenating whole copies is a valid transport stream
  // (it is a packet sequence, not a container with a trailer) and gives a
  // realistically sized input without committing a large binary fixture.
  const std::size_t target_bytes = target_mib * 1024 * 1024;
  std::vector<std::byte> ts_repeated;
  ts_repeated.reserve(target_bytes + ts.size());
  while (ts_repeated.size() < target_bytes)
    ts_repeated.insert(ts_repeated.end(), ts.begin(), ts.end());

  const Registry* reg = registry_for(RegistryId::Uas0601);
  if (!reg) {
    std::fprintf(stderr, "no 0601 registry\n");
    return 2;
  }

  std::printf("libmisbklv bench — median of repeated runs, single thread\n\n");
  bench_codec(*reg);
  std::printf("\n");
  bench_message(klv);
  std::printf("\n");
  bench_ts_extract(ts_repeated);
#ifdef MISBKLV_BENCH_GST
  std::printf("\n");
  bench_gst_extract(ts_repeated);
#endif
  std::printf("\nsink=%llu (ignore; keeps the work live)\n",
              static_cast<unsigned long long>(g_sink));
  return 0;
}
