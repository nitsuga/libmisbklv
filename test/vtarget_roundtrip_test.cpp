// SPDX-License-Identifier: Apache-2.0
// Milestone 6: VTarget Series round-trip. A VMTI packet carrying Item 101
// (vTargetSeries) of VTarget Packs. Validates ST 0903 §9.1.3 Series parsing,
// the VTarget Pack (BER-OID id + LS), childRegistry routing into the VTarget
// registry, IMAPB inside packs, and byte-exact Series + packet rebuild.
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
#include "misbklv/series.hpp"
#include "misbklv/types.hpp"

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

// Reserialize items against a registry (typed codec where registered, else raw),
// preserving source lengths -> bare TLV bytes.
static ber::Bytes rebuild_items(const Registry& reg, const std::vector<Item>& items) {
  LocalSetBuilder b(reg);
  for (const auto& it : items) {
    const ItemDescriptor* d = reg.find(it.tag);
    if (d && d->kind != ValueKind::NestedLS && d->kind != ValueKind::Pack) {
      auto v = codec::decode(*d, it.value);
      (void)b.set(it.tag, *v, it.value.size());
    } else {
      b.append_raw(it.tag, it.value);
    }
  }
  return b.serialize_items();
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "test/fixtures/vmti_vtarget.klv";
  const std::vector<std::byte> data = read_file(path);
  std::printf("fixture: %s (%zu bytes)\n", path, data.size());

  auto pkt = parse_packet(data);
  if (!pkt) { std::fprintf(stderr, "parse failed\n"); return 2; }
  const Registry* vmti = registry_by_key(pkt->ul_key);
  if (vmti != &gen::vmti_0903) { std::fprintf(stderr, "not VMTI\n"); return 2; }

  // locate Item 101 and route to the VTarget registry
  const Item* v101 = nullptr;
  for (const auto& it : pkt->items) if (it.tag == 101) v101 = &it;
  if (!v101) { std::fprintf(stderr, "no Item 101\n"); return 2; }
  const ItemDescriptor* d101 = vmti->find(101);
  if (!d101 || d101->kind != ValueKind::Pack) { std::fprintf(stderr, "101 not Pack\n"); return 2; }
  const Registry* vt = registry_for(d101->child);
  if (vt != &gen::vtarget_0903) { std::fprintf(stderr, "child routing wrong\n"); return 2; }
  std::printf("Item 101 -> Series of '%s' packs (routing OK)\n", std::string(vt->id).c_str());

  // --- parse the Series --------------------------------------------------
  auto packs = parse_vtarget_series(v101->value);
  if (!packs) { std::fprintf(stderr, "series parse failed\n"); return 2; }
  std::printf("vTargetSeries: %zu VTarget packs\n", packs->size());
  for (const auto& p : *packs) {
    std::printf("  target %llu:\n", static_cast<unsigned long long>(p.target_id));
    for (const auto& it : p.items) {
      const ItemDescriptor* d = vt->find(it.tag);
      if (!d) continue;
      auto v = codec::decode(*d, it.value);
      if (d->kind == ValueKind::IMAPB)
        std::printf("    tag %2u  %-24s = %.4f %s\n", it.tag, std::string(d->name).c_str(),
                    std::get<double>(*v), std::string(d->units).c_str());
      else
        std::printf("    tag %2u  %-24s = %llu\n", it.tag, std::string(d->name).c_str(),
                    static_cast<unsigned long long>(std::get<std::uint64_t>(*v)));
    }
  }

  // --- rebuild the Series, then the packet, byte-exact -------------------
  std::vector<ber::Bytes> elements;
  for (const auto& p : *packs)
    elements.push_back(build_vtarget_pack(p.target_id, rebuild_items(*vt, p.items)));
  ber::Bytes series = build_series(elements);
  const bool series_ok = series.size() == v101->value.size() &&
                         std::equal(series.begin(), series.end(), v101->value.begin());
  std::printf("vTargetSeries re-encode: %s\n", series_ok ? "byte-exact" : "MISMATCH");

  LocalSetBuilder b(*vmti);
  for (const auto& it : pkt->items) {
    if (it.tag == 1) continue;
    if (it.tag == 101) { b.append_raw(101, series); continue; }
    const ItemDescriptor* d = vmti->find(it.tag);
    if (d) { auto v = codec::decode(*d, it.value); (void)b.set(it.tag, *v, it.value.size()); }
    else b.append_raw(it.tag, it.value);
  }
  auto rebuilt = std::move(b).finalize(vmti->ul_key, /*enforce_mandatory=*/false);
  if (!rebuilt) { std::fprintf(stderr, "finalize failed\n"); return 2; }
  const bool pkt_ok = (*rebuilt == data);
  std::printf("packet re-encode: %s\n", pkt_ok ? "byte-exact" : "MISMATCH");
  std::printf("VTARGET ROUND-TRIP: %s\n", (series_ok && pkt_ok) ? "PASS" : "FAIL");
  return (series_ok && pkt_ok) ? 0 : 1;
}
