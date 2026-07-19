// SPDX-License-Identifier: Apache-2.0
// GstBackend extraction test (ADR 0013 / B1): extract KLV from an MPEG-TS via
// gstreamer, frame packets, and assert byte-exact against the fixture the core
// already round-trips.  argv: <source.ts> <expected.klv>
#include <cstdio>
#include <fstream>
#include <span>
#include <vector>

#include "misbklv/gst_backend.hpp"
#include "misbklv/packet.hpp"

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
    std::fprintf(stderr, "usage: gst_backend_test <source.ts> <expected.klv>\n");
    return 2;
  }
  auto be = make_gst_backend();
  std::vector<std::byte> extracted;
  std::size_t npkt = 0;
  bool all_parse = true;
  auto r = be->extract(argv[1], [&](const KlvPacket& kp) {
    extracted.insert(extracted.end(), kp.bytes.begin(), kp.bytes.end());
    ++npkt;
    if (!parse_packet(kp.bytes)) all_parse = false;  // each unit is a whole packet
  });
  if (!r) {
    std::fprintf(stderr, "extract failed: error %d\n", static_cast<int>(r.error()));
    return 2;
  }
  const auto expected = read_file(argv[2]);
  std::printf("extracted %zu packets, %zu bytes (expected %zu)\n", npkt,
              extracted.size(), expected.size());
  std::printf("every unit is a whole KLV packet: %s\n", all_parse ? "yes" : "no");
  const bool match = (extracted == expected);
  std::printf("GST EXTRACT vs fixture: %s\n", match ? "byte-exact PASS" : "MISMATCH");
  return (match && all_parse && npkt > 0) ? 0 : 1;
}
