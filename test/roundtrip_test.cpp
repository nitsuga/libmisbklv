// SPDX-License-Identifier: Apache-2.0
// Byte-exact round-trip of the generated synthetic ST 0601 packet.
// Decode -> re-encode via the builder -> assert identical bytes. Acceptance
// gate for ADRs 0010 (descriptor schema) + 0011 (encode model).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <span>
#include <vector>

#include "misbklv/builder.hpp"
#include "misbklv/codec.hpp"
#include "misbklv/packet.hpp"
#include "misbklv/types.hpp"
#include "misbklv/registry/uas0601_tables.generated.hpp"

using namespace misbklv;

static std::vector<std::byte> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i)
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: roundtrip_test <input.klv>\n");
    return 2;
  }
  const char* path = argv[1];
  const std::vector<std::byte> original = read_file(path);
  if (original.empty()) {
    std::fprintf(stderr, "could not read fixture: %s\n", path);
    return 2;
  }
  std::printf("fixture: %s (%zu bytes)\n", path, original.size());

  // --- decode ---------------------------------------------------------------
  auto parsed = parse_packet(original);
  if (!parsed) {
    std::fprintf(stderr, "parse failed: error %d\n", static_cast<int>(parsed.error()));
    return 2;
  }
  std::printf("parsed %zu items\n", parsed->items.size());
  for (const auto& it : parsed->items) {
    const ItemDescriptor* d = gen::uas_0601.find(it.tag);
    if (!d) continue;
    auto v = codec::decode(*d, it.value);
    if (!v) continue;
    if (d->kind == ValueKind::LinearLDS)
      std::printf("  tag %3u  %-24s = %.6f %s\n", it.tag, std::string(d->name).c_str(),
                  std::get<double>(*v), std::string(d->units).c_str());
    else if (d->kind == ValueKind::UInt)
      std::printf("  tag %3u  %-24s = %llu\n", it.tag, std::string(d->name).c_str(),
                  static_cast<unsigned long long>(std::get<std::uint64_t>(*v)));
  }

  // --- re-encode (registered items via typed codec; others raw) -------------
  LocalSetBuilder b(gen::uas_0601);
  for (const auto& it : parsed->items) {
    if (it.tag == 1) continue;  // checksum is emitted by finalize()
    const ItemDescriptor* d = gen::uas_0601.find(it.tag);
    if (d) {
      auto v = codec::decode(*d, it.value);
      auto r = b.set(it.tag, *v, it.value.size());  // preserve source length
      if (!r) {
        std::fprintf(stderr, "set(tag %u) failed: %d\n", it.tag, static_cast<int>(r.error()));
        return 2;
      }
    } else {
      b.append_raw(it.tag, it.value);
    }
  }
  auto rebuilt = std::move(b).finalize(std::span{kUas0601Key});
  if (!rebuilt) {
    std::fprintf(stderr, "finalize failed: %d\n", static_cast<int>(rebuilt.error()));
    return 2;
  }

  // --- compare --------------------------------------------------------------
  std::printf("rebuilt: %zu bytes\n", rebuilt->size());
  if (*rebuilt == original) {
    std::printf("ROUND-TRIP: byte-exact PASS\n");
    return 0;
  }
  std::printf("ROUND-TRIP: MISMATCH\n");
  const std::size_t n = std::min(rebuilt->size(), original.size());
  for (std::size_t i = 0; i < n; ++i)
    if ((*rebuilt)[i] != original[i]) {
      std::printf("  first diff at offset %zu: got 0x%02X want 0x%02X\n", i,
                  std::to_integer<unsigned>((*rebuilt)[i]), std::to_integer<unsigned>(original[i]));
      break;
    }
  if (rebuilt->size() != original.size())
    std::printf("  size differs: %zu vs %zu\n", rebuilt->size(), original.size());
  return 1;
}
