// SPDX-License-Identifier: Apache-2.0
// Milestone 3: nested Local Set round-trip. An ST 0601 packet nests an ST 0903
// VMTI LS in Item 74. Validates childRegistry routing (ADR 0010) and recursive
// parse + build both directions (ADR 0005/0011).
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
  std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i)
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}

// Reserialize a flat set of items against `reg` (typed codec where registered,
// raw otherwise), preserving source lengths. Returns the bare TLV bytes.
static ber::Bytes rebuild_items(const Registry& reg,
                                const std::vector<Item>& items) {
  LocalSetBuilder b(reg);
  for (const auto& it : items) {
    const ItemDescriptor* d = reg.find(it.tag);
    if (d && d->kind != ValueKind::NestedLS) {
      auto v = codec::decode(*d, it.value);
      (void)b.set(it.tag, *v, it.value.size());
    } else {
      b.append_raw(it.tag, it.value);
    }
  }
  return b.serialize_items();
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "test/fixtures/vmti_nested.klv";
  const std::vector<std::byte> data = read_file(path);
  std::printf("fixture: %s (%zu bytes)\n", path, data.size());

  auto outer = parse_packet(data);
  if (!outer) {
    std::fprintf(stderr, "outer parse failed: %d\n", static_cast<int>(outer.error()));
    return 2;
  }

  // --- locate Item 74 and route to its child registry (ADR 0010) ------------
  const Item* v74 = nullptr;
  for (const auto& it : outer->items)
    if (it.tag == 74) v74 = &it;
  if (!v74) {
    std::fprintf(stderr, "no Item 74 (VMTI LS) in packet\n");
    return 2;
  }
  const ItemDescriptor* d74 = gen::uas_0601.find(74);
  if (!d74 || d74->kind != ValueKind::NestedLS) {
    std::fprintf(stderr, "Item 74 descriptor not NestedLS\n");
    return 2;
  }
  const Registry* child = registry_for(d74->child);
  if (child != &gen::vmti_0903) {
    std::fprintf(stderr, "childRegistry routing wrong\n");
    return 2;
  }
  std::printf("Item 74 -> child registry '%s' (routing OK)\n",
              std::string(child->id).c_str());

  // --- recursively parse the nested VMTI LS ---------------------------------
  auto vmti = parse_items(v74->value);
  if (!vmti) {
    std::fprintf(stderr, "nested parse failed: %d\n", static_cast<int>(vmti.error()));
    return 2;
  }
  std::printf("nested VMTI LS: %zu items\n", vmti->size());
  for (const auto& it : *vmti) {
    const ItemDescriptor* d = child->find(it.tag);
    if (!d) continue;
    auto v = codec::decode(*d, it.value);
    if (d->kind == ValueKind::IMAPB || d->kind == ValueKind::LinearLDS)
      std::printf("  VMTI tag %2u  %-32s = %.4f %s\n", it.tag,
                  std::string(d->name).c_str(), std::get<double>(*v),
                  std::string(d->units).c_str());
    else
      std::printf("  VMTI tag %2u  %-32s = %llu\n", it.tag,
                  std::string(d->name).c_str(),
                  static_cast<unsigned long long>(std::get<std::uint64_t>(*v)));
  }

  // --- recursive rebuild: inner set, then outer packet ----------------------
  ber::Bytes inner = rebuild_items(*child, *vmti);
  const bool inner_ok =
      inner.size() == v74->value.size() &&
      std::equal(inner.begin(), inner.end(), v74->value.begin());
  std::printf("nested VMTI re-encode: %s\n", inner_ok ? "byte-exact" : "MISMATCH");

  LocalSetBuilder ob(gen::uas_0601);
  for (const auto& it : outer->items) {
    if (it.tag == 1) continue;
    if (it.tag == 74) {
      ob.append_raw(74, inner);  // the rebuilt nested set
    } else {
      const ItemDescriptor* d = gen::uas_0601.find(it.tag);
      if (d) {
        auto v = codec::decode(*d, it.value);
        (void)ob.set(it.tag, *v, it.value.size());
      } else {
        ob.append_raw(it.tag, it.value);
      }
    }
  }
  auto rebuilt = std::move(ob).finalize(std::span{kUas0601Key},
                                        /*enforce_mandatory=*/false);
  if (!rebuilt) {
    std::fprintf(stderr, "outer finalize failed: %d\n",
                 static_cast<int>(rebuilt.error()));
    return 2;
  }
  const bool outer_ok = (*rebuilt == data);
  std::printf("outer packet re-encode: %s\n", outer_ok ? "byte-exact" : "MISMATCH");
  std::printf("NESTED ROUND-TRIP: %s\n", (inner_ok && outer_ok) ? "PASS" : "FAIL");
  return (inner_ok && outer_ok) ? 0 : 1;
}
