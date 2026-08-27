// SPDX-License-Identifier: Apache-2.0
// Milestone 5: standalone ST 0903 VMTI round-trip. A VMTI LS as its own
// top-level packet (its own UL key + checksum), distinct from the embedded
// (0601 Item 74) form. Validates UL-key -> registry dispatch (registry_by_key)
// and that finalize()'s checksum is correct for the VMTI key (ST 0903 0903.6-119).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "misbklv/builder.hpp"
#include "misbklv/codec.hpp"
#include "misbklv/packet.hpp"
#include "misbklv/registries.hpp"
#include "misbklv/types.hpp"

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
  const char* path = argc > 1 ? argv[1] : "test/fixtures/vmti_standalone.klv";
  const std::vector<std::byte> data = read_file(path);
  std::printf("fixture: %s (%zu bytes)\n", path, data.size());

  auto pkt = parse_packet(data);
  if (!pkt) {
    std::fprintf(stderr, "parse failed: %d\n", static_cast<int>(pkt.error()));
    return 2;
  }

  // --- demux dispatch: pick the registry from the UL key --------------------
  const Registry* reg = registry_by_key(pkt->ul_key);
  if (reg != &gen::vmti_0903) {
    std::fprintf(stderr, "registry_by_key did not resolve to VMTI\n");
    return 2;
  }
  if (registry_by_key(pkt->ul_key) == &gen::uas_0601) {
    std::fprintf(stderr, "VMTI key wrongly matched 0601\n");
    return 2;
  }
  std::printf("UL key -> registry '%s' (standalone dispatch OK)\n", std::string(reg->id).c_str());

  // --- decode ---------------------------------------------------------------
  std::printf("%zu items\n", pkt->items.size());
  for (const auto& it : pkt->items) {
    const ItemDescriptor* d = reg->find(it.tag);
    if (!d) continue;
    auto v = codec::decode(*d, it.value);
    if (d->kind == ValueKind::IMAPB || d->kind == ValueKind::LinearLDS)
      std::printf("  tag %2u  %-32s = %.4f %s\n", it.tag, std::string(d->name).c_str(),
                  std::get<double>(*v), std::string(d->units).c_str());
    else if (d->kind == ValueKind::UInt)
      std::printf("  tag %2u  %-32s = %llu\n", it.tag, std::string(d->name).c_str(),
                  static_cast<unsigned long long>(std::get<std::uint64_t>(*v)));
  }

  // --- re-encode as a standalone packet (own key + checksum) ----------------
  LocalSetBuilder b(*reg);
  for (const auto& it : pkt->items) {
    if (it.tag == 1) continue;  // checksum re-emitted by finalize()
    const ItemDescriptor* d = reg->find(it.tag);
    if (d && d->kind != ValueKind::NestedLS) {
      auto v = codec::decode(*d, it.value);
      (void)b.set(it.tag, *v, it.value.size());
    } else {
      b.append_raw(it.tag, it.value);
    }
  }
  auto rebuilt = std::move(b).finalize(reg->ul_key, /*enforce_mandatory=*/false);
  if (!rebuilt) {
    std::fprintf(stderr, "finalize failed: %d\n", static_cast<int>(rebuilt.error()));
    return 2;
  }
  const bool ok = (*rebuilt == data);
  std::printf("standalone re-encode: %s\n", ok ? "byte-exact" : "MISMATCH");
  std::printf("STANDALONE ROUND-TRIP: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
