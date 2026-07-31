// SPDX-License-Identifier: Apache-2.0
// Minimal libmisbklv example: read KLV from an MPEG-TS, nudge each frame's
// Sensor Latitude, and write a new MPEG-TS. Shows the whole high-level facade
// (ADR 0018): KlvStream -> typed get/set on Message -> KlvSink.
//
//   klv_edit <in.ts> <out.ts>
#include <cstdio>
#include <string>

#include "misbklv/stream.hpp"

using namespace misbklv;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <in.ts> <out.ts>\n", argv[0]);
    return 2;
  }
  constexpr auto kSensorLatitude = tags::Uas0601::SensorLatitude;  // named ST 0601 tag

  KlvStream in(argv[1]);
  KlvSink out(std::string("file:") + argv[2]);
  if (auto error = out.error()) {
    std::fprintf(stderr, "open output failed: %d\n", static_cast<int>(*error));
    return 1;
  }

  std::size_t n = 0;
  for (Message& msg : in) {
    if (auto lat = msg.get<double>(kSensorLatitude)) {
      auto edited = msg.set(kSensorLatitude, Value{*lat + 0.001});  // ~100 m north
      if (!edited) {
        std::fprintf(stderr, "edit failed: %d\n", static_cast<int>(edited.error()));
        return 1;
      }
    }
    auto emitted = out.emit(msg);
    if (!emitted) {
      std::fprintf(stderr, "emit failed: %d\n", static_cast<int>(emitted.error()));
      return 1;
    }
    ++n;
  }
  if (auto error = in.error()) {
    std::fprintf(stderr, "read failed: %d\n", static_cast<int>(*error));
    return 1;
  }
  auto closed = out.close();
  if (!closed) {
    std::fprintf(stderr, "close failed: %d\n", static_cast<int>(closed.error()));
    return 1;
  }
  std::printf("processed %zu KLV packet(s) -> %s\n", n, argv[2]);
  return 0;
}
