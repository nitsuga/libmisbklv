// SPDX-License-Identifier: Apache-2.0
// gst-free MPEG-TS KLV extractor test (fork 12 / B3). Extract KLV from a .ts
// (0x06 or 0x15) and assert byte-exact against a reference ES. argv:
// <source.ts> <expected.klv>
#include <cstdio>
#include <fstream>
#include <span>
#include <vector>

#include "misbklv/packet.hpp"
#include "misbklv/ts.hpp"

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

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: ts_extract_test <source.ts> <expected.klv>\n");
    return 2;
  }
  const auto ts = read_file(argv[1]);
  std::vector<std::byte> out;
  std::size_t npkt = 0;
  bool all_parse = true;
  auto r = extract_ts_klv(ts, [&](const KlvPacket& kp) {
    out.insert(out.end(), kp.bytes.begin(), kp.bytes.end());
    ++npkt;
    if (!parse_packet(kp.bytes)) all_parse = false;
  });
  if (!r) {
    std::fprintf(stderr, "no KLV PID found: error %d\n", static_cast<int>(r.error()));
    return 2;
  }
  const auto expected = read_file(argv[2]);
  std::printf("extracted %zu packets, %zu bytes (expected %zu)\n", npkt,
              out.size(), expected.size());
  std::printf("every unit is a whole KLV packet: %s\n", all_parse ? "yes" : "no");
  const bool match = (out == expected);
  std::printf("TS EXTRACT vs reference: %s\n", match ? "byte-exact PASS" : "MISMATCH");
  return (match && all_parse && npkt > 0) ? 0 : 1;
}
