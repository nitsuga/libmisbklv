// SPDX-License-Identifier: Apache-2.0
// Proves the generated, project-owned corpus retains the breadth once supplied
// by third-party captures. argv: <basic.klv> <comprehensive.klv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <span>
#include <vector>

#include "misbklv/codec.hpp"
#include "misbklv/packet.hpp"
#include "misbklv/registry/uas0601_tables.generated.hpp"

using namespace misbklv;

static int failures = 0;

static void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}

static std::vector<std::byte> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i)
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}

static std::vector<Packet> parse_stream(const std::vector<std::byte>& bytes) {
  std::vector<Packet> packets;
  std::span<const std::byte> stream = bytes;
  for (std::size_t off = 0; off < bytes.size();) {
    auto parsed = parse_packet(stream.subspan(off));
    if (!parsed) return {};
    off += parsed->total_size;
    packets.push_back(std::move(*parsed));
  }
  return packets;
}

static const Item* find(const Packet& packet, std::uint16_t tag) {
  for (const auto& item : packet.items)
    if (item.tag == tag) return &item;
  return nullptr;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: synthetic_fixture_test <basic.klv> <comprehensive.klv>\n");
    return 2;
  }

  const auto basic_bytes = read_file(argv[1]);
  const auto broad_bytes = read_file(argv[2]);
  const auto basic = parse_stream(basic_bytes);
  const auto broad = parse_stream(broad_bytes);
  check(basic.size() == 6, "basic stream has six time-correlated packets");
  check(broad.size() == 3, "comprehensive stream has ordinary, special, and width packets");
  if (basic.size() != 6 || broad.size() != 3) return 1;

  std::uint64_t previous_timestamp = 0;
  bool timestamps_ok = true, latitude_ok = true;
  for (std::size_t i = 0; i < basic.size(); ++i) {
    const auto* timestamp = find(basic[i], 2);
    const auto* latitude = find(basic[i], 13);
    if (!timestamp || !latitude) {
      timestamps_ok = latitude_ok = false;
      continue;
    }
    const auto decoded_ts = codec::decode(*gen::uas_0601.find(2), timestamp->value);
    const auto decoded_lat = codec::decode(*gen::uas_0601.find(13), latitude->value);
    if (!decoded_ts || !std::holds_alternative<std::uint64_t>(*decoded_ts) ||
        (i && std::get<std::uint64_t>(*decoded_ts) - previous_timestamp != 100'000))
      timestamps_ok = false;
    else
      previous_timestamp = std::get<std::uint64_t>(*decoded_ts);
    if (!decoded_lat || !std::holds_alternative<double>(*decoded_lat) ||
        std::fabs(std::get<double>(*decoded_lat) - (12.5 + i * 0.01)) > 0.001)
      latitude_ok = false;
  }
  check(timestamps_ok, "invented timestamps advance by 100 ms");
  check(latitude_ok, "invented editable latitude values decode as authored");

  std::set<std::uint16_t> ordinary_tags;
  for (const auto& item : broad[0].items) ordinary_tags.insert(item.tag);
  bool every_descriptor = true;
  for (const auto& descriptor : gen::uas_0601.items)
    if (descriptor.tag != 1 && !ordinary_tags.contains(descriptor.tag)) every_descriptor = false;
  check(every_descriptor, "ordinary packet covers every registered ST 0601 item");

  bool every_special = true;
  for (const auto& descriptor : gen::uas_0601.items) {
    if (descriptor.specials.empty()) continue;
    const auto* item = find(broad[1], descriptor.tag);
    if (!item || codec::rd_uint(item->value) != descriptor.specials.front().pattern)
      every_special = false;
  }
  check(every_special, "special packet covers every registered special bit pattern family");

  bool widths_ok = true;
  for (const auto& descriptor : gen::uas_0601.items) {
    if (!descriptor.variable ||
        (descriptor.kind != ValueKind::UInt && descriptor.kind != ValueKind::Int &&
         descriptor.kind != ValueKind::IMAPB))
      continue;
    const auto* item = find(broad[2], descriptor.tag);
    if (!item || item->value.empty() || item->value.size() >= descriptor.fixed_len)
      widths_ok = false;
  }
  check(widths_ok, "variable numeric items use valid non-default widths");
  check(find(broad[2], 150) != nullptr,
        "multi-byte BER-OID unregistered item is retained for raw passthrough");

  std::printf("%s\n", failures ? "SYNTHETIC FIXTURES: FAIL" : "SYNTHETIC FIXTURES: all PASS");
  return failures ? 1 : 0;
}
