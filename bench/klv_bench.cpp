// SPDX-License-Identifier: Apache-2.0
// Timing harness for the paths issue #50 parks work behind. Not a test: it
// asserts nothing and is not registered with CTest, because a timing threshold
// in CI measures the runner more than the code. It is built by default so it
// cannot bit-rot, and run by hand when a number is needed.
//
// Reports the median of several reps rather than the mean: a background process
// on the machine skews the mean and leaves the median alone.
//
// Covers the core paths only. A gst-extraction row was tried and removed: the
// only way this harness can reach a realistic input size is to concatenate
// copies of a ~1.5 KiB fixture, which restarts continuity counters, PCR and PTS
// at every boundary. `extract_ts_klv` is indifferent to that — it selects the
// KLV PID by content and never reads a continuity counter — but a demuxer is
// not, and measured recovery from thousands of injected discontinuities is not
// steady-state demux. A representative gst number needs a fixture generated at
// length with coherent CC/PCR/PTS, which belongs in the fixture generator
// rather than here (issue #50).
//
// argv: <fixture.klv> <fixture.ts> [target_MiB]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "misbklv/codec.hpp"
#include "misbklv/message.hpp"
#include "misbklv/registries.hpp"
#include "misbklv/ts.hpp"

using namespace misbklv;

namespace {

// Kept live so the optimizer cannot delete the work being timed.
volatile std::uint64_t g_sink = 0;

std::vector<std::byte> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i)
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}

// Median nanoseconds per iteration over `reps` runs of `iters` iterations.
template <class F> double median_ns_per_iter(int reps, std::size_t iters, F&& body) {
  // Warm up before the clock starts, so the first rep is not paying for cold
  // caches and a ramping clock. This does not make the rows stable — codec rows
  // still move around 15% run to run on an ordinary desktop — so read the output
  // as ranges across several runs, not as point values.
  for (std::size_t i = 0; i < std::min<std::size_t>(iters, 1000); ++i) body();
  std::vector<double> per_iter;
  per_iter.reserve(reps);
  for (int r = 0; r < reps; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iters; ++i) body();
    const auto t1 = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    per_iter.push_back(static_cast<double>(ns) / static_cast<double>(iters));
  }
  std::sort(per_iter.begin(), per_iter.end());
  return per_iter[per_iter.size() / 2];
}

void report(const char* what, double ns) {
  std::printf("  %-42s %10.1f ns/op\n", what, ns);
}

void report_throughput(const char* what, double ns, std::size_t bytes) {
  const double mib_per_s = (static_cast<double>(bytes) / (1024.0 * 1024.0)) / (ns / 1e9);
  std::printf("  %-42s %10.1f ns/op  %8.1f MiB/s\n", what, ns, mib_per_s);
}

// An item of `kind` whose encode width the numeric codecs accept, preferring
// `want_width` so kinds can be compared without a width confound.
//
// Width matters more than it looks: rd_uint loops over the bytes, so an 8-byte
// item costs materially more than a 1-byte one *within* a kind. Comparing a
// 3-byte IMAPB against a 2-byte LinearLDS would therefore fold width into what
// is supposed to be a per-kind number. The 0601 registry happens to offer a
// 2-byte item for UInt, LinearLDS and IMAPB, so those three are directly
// comparable; Int has only 1, 4 and 8-byte items and is reported at its own
// width, labeled as such.
//
// `variable` is deliberately not filtered on: every IMAPB item in 0601 is
// variable-width with a `fixed_len` default, so excluding them would drop the
// one kind this benchmark exists to measure. `fixed_len` is the encode width
// either way (see ItemDescriptor). Tag 1 is skipped — the checksum, which
// Message::set() rejects as read-only, and the edit row below reuses this.
const ItemDescriptor* first_of_kind(const Registry& reg, ValueKind kind,
                                    std::uint8_t want_width = 0) {
  const ItemDescriptor* fallback = nullptr;
  for (const auto& d : reg.items) {
    if (d.kind != kind || d.tag == 1 || d.fixed_len < 1 || d.fixed_len > 8) continue;
    if (want_width && d.fixed_len == want_width) return &d;
    if (!fallback) fallback = &d;
  }
  return fallback;
}

// --- (1) per-kind codec round trip ------------------------------------------
// The measurement issue #49 is gated on. IMAPB recomputes ceil/log2/exp2 per
// item and LinearLDS does not, while both decode to a double and re-encode, so
// with the width held equal the gap between those two rows is what precomputing
// the ST 1201 scaling factors could remove.
void bench_codec(const Registry& reg) {
  // 2 bytes: the one width UInt, LinearLDS and IMAPB all offer in 0601.
  constexpr std::uint8_t kWantWidth = 2;
  std::printf("codec decode+encode, one item, %u-byte where the registry allows\n",
              static_cast<unsigned>(kWantWidth));
  const ValueKind kinds[] = {ValueKind::UInt, ValueKind::Int, ValueKind::LinearLDS,
                             ValueKind::IMAPB};
  const char* names[] = {"UInt", "Int", "LinearLDS", "IMAPB"};
  for (std::size_t k = 0; k < std::size(kinds); ++k) {
    const ItemDescriptor* d = first_of_kind(reg, kinds[k], kWantWidth);
    if (!d) {
      std::printf("  %-42s (no item of this kind in registry)\n", names[k]);
      continue;
    }
    // A mid-range payload: the top two bits stay clear so an IMAPB item is a
    // normal mapped value rather than a structural special (ST 1201 7.2.3),
    // which would short-circuit the very math being measured.
    std::vector<std::byte> raw(d->fixed_len, std::byte{0x11});
    raw[0] = std::byte{0x40};

    char label[96];
    std::snprintf(label, sizeof(label), "%s (tag %u, %u bytes)%s", names[k],
                  static_cast<unsigned>(d->tag), static_cast<unsigned>(d->fixed_len),
                  d->fixed_len == kWantWidth ? "" : "  <- width differs");
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
  report_throughput("Message::encode (unedited passthrough)", encode_ns, klv.size());

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
                static_cast<unsigned>(uint_item->tag), static_cast<int>(set_ok.error()));
    return;
  }
  const double reencode_ns = median_ns_per_iter(5, 20000, [&] {
    auto out = edited->encode();
    if (out) g_sink += out->size();
  });
  report_throughput("Message::encode (one edit, rebuilds)", reencode_ns, klv.size());
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
  report_throughput("extract_ts_klv (2 passes: origin + frame)", ns, ts_repeated.size());
  std::printf("  (%zu packets extracted)\n", packets);
}

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
  std::printf("\nsink=%llu (ignore; keeps the work live)\n",
              static_cast<unsigned long long>(g_sink));
  return 0;
}
