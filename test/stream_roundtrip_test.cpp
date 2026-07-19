// SPDX-License-Identifier: Apache-2.0
// Milestone 2: full-stream byte-exact round-trip. Walks every KLV packet in a
// sample stream, re-encodes each (typed codec for registered items, raw for the
// rest, special-value patterns passed raw), and asserts the whole reconstructed
// stream is byte-identical. Also reports the tag histogram to drive registry work.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "misbklv/builder.hpp"
#include "misbklv/codec.hpp"
#include "misbklv/packet.hpp"
#include "misbklv/types.hpp"
#include "misbklv/registry/uas0601_tables.generated.hpp"

using namespace misbklv;

static std::vector<std::byte> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i)
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}

// True if `raw` matches one of the item's special-value bit patterns (ADR 0010).
static bool is_special(const ItemDescriptor& d, std::span<const std::byte> raw) {
  const std::uint64_t v = codec::rd_uint(raw);
  for (const auto& s : d.specials)
    if (s.pattern == v) return true;
  return false;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: stream_roundtrip_test <stream.klv>\n");
    return 2;
  }
  const std::vector<std::byte> data = read_file(argv[1]);
  std::span<const std::byte> buf = data;
  std::printf("stream: %s (%zu bytes)\n", argv[1], data.size());

  std::vector<std::byte> rebuilt_stream;
  std::map<std::uint16_t, int> tag_hist;
  std::set<std::uint16_t> unregistered;
  std::size_t off = 0, npkt = 0, npass = 0;
  bool all_ok = true;

  while (off < data.size()) {
    auto pkt = parse_packet(buf.subspan(off));
    if (!pkt) {
      std::fprintf(stderr, "packet %zu: parse error %d at off %zu\n", npkt,
                   static_cast<int>(pkt.error()), off);
      return 2;
    }
    const std::size_t sz = pkt->total_size;
    std::span<const std::byte> original = buf.subspan(off, sz);

    LocalSetBuilder b(gen::uas_0601);
    for (const auto& it : pkt->items) {
      if (it.tag != 1) tag_hist[it.tag]++;  // checksum re-emitted by finalize
      if (it.tag == 1) continue;
      const ItemDescriptor* d = gen::uas_0601.find(it.tag);
      if (d && !is_special(*d, it.value)) {
        auto v = codec::decode(*d, it.value);
        (void)b.set(it.tag, *v, it.value.size());  // preserve source length
      } else {
        if (!d) unregistered.insert(it.tag);
        b.append_raw(it.tag, it.value);
      }
    }
    auto rb = std::move(b).finalize(std::span{kUas0601Key}, /*enforce_mandatory=*/false);
    if (!rb) {
      std::fprintf(stderr, "packet %zu: finalize error %d\n", npkt,
                   static_cast<int>(rb.error()));
      return 2;
    }
    const bool match =
        rb->size() == sz &&
        std::equal(rb->begin(), rb->end(), original.begin());
    ++npkt;
    if (match) {
      ++npass;
    } else {
      all_ok = false;
      std::printf("  packet %zu (%zu B): MISMATCH\n", npkt - 1, sz);
      for (std::size_t i = 0; i < std::min(rb->size(), sz); ++i)
        if ((*rb)[i] != original[i]) {
          std::printf("    first diff at +%zu: got 0x%02X want 0x%02X\n", i,
                      std::to_integer<unsigned>((*rb)[i]),
                      std::to_integer<unsigned>(original[i]));
          break;
        }
    }
    rebuilt_stream.insert(rebuilt_stream.end(), rb->begin(), rb->end());
    off += sz;
  }

  std::printf("packets: %zu, per-packet byte-exact: %zu/%zu\n", npkt, npass, npkt);
  std::printf("tags seen (%zu distinct):\n", tag_hist.size());
  for (auto [tag, cnt] : tag_hist) {
    const ItemDescriptor* d = gen::uas_0601.find(tag);
    std::printf("  tag %3u  x%-3d  %s%s\n", tag, cnt,
                d ? std::string(d->name).c_str() : "(unregistered)",
                d ? "" : "  [RAW]");
  }
  std::printf("unregistered tags round-tripped raw: %zu\n", unregistered.size());

  const bool full = (rebuilt_stream == data);
  std::printf("FULL-STREAM byte-exact: %s\n", full ? "PASS" : "FAIL");
  return (all_ok && full) ? 0 : 1;
}
