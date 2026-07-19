// SPDX-License-Identifier: Apache-2.0
// Insertion round-trip (ADR 0013 / B2): KLV packets -> appsrc ! mpegtsmux !
// filesink -> .ts -> re-extract -> byte-exact. Proves stock mpegtsmux carries
// KLV (0x06+KLVA) losslessly, so no klvpmtrewrite is needed.
// argv: <input.klv> <temp.ts>
#include <cstdio>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "misbklv/backend.hpp"
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
    std::fprintf(stderr, "usage: gst_insert_test <input.klv> <temp.ts>\n");
    return 2;
  }
  const auto input = read_file(argv[1]);
  std::span<const std::byte> buf = input;
  auto be = make_gst_backend();

  // --- insert: push each framed KLV packet through the mux to a .ts ---------
  auto ins = be->open_insert({std::string("file:") + argv[2]});
  if (!ins) {
    std::fprintf(stderr, "open_insert failed: %d\n", static_cast<int>(ins.error()));
    return 2;
  }
  std::size_t off = 0, npush = 0;
  while (off < input.size()) {
    const std::size_t n = packet_frame_length(buf.subspan(off));
    if (n == 0) break;
    if (!(*ins)->push(buf.subspan(off, n), kNoPts)) {
      std::fprintf(stderr, "push failed at packet %zu\n", npush);
      return 2;
    }
    off += n;
    ++npush;
  }
  if (!(*ins)->finish()) {
    std::fprintf(stderr, "finish failed\n");
    return 2;
  }
  std::printf("inserted %zu packets -> %s\n", npush, argv[2]);

  // --- re-extract and compare ----------------------------------------------
  std::vector<std::byte> out;
  auto r = be->extract(argv[2], [&](const KlvPacket& kp) {
    out.insert(out.end(), kp.bytes.begin(), kp.bytes.end());
  });
  if (!r) {
    std::fprintf(stderr, "re-extract failed: %d\n", static_cast<int>(r.error()));
    return 2;
  }
  std::printf("re-extracted %zu bytes (input %zu)\n", out.size(), input.size());
  const bool match = (out == input);
  std::printf("INSERT ROUND-TRIP: %s\n", match ? "byte-exact PASS" : "MISMATCH");
  return match ? 0 : 1;
}
