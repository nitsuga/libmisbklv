// SPDX-License-Identifier: Apache-2.0
// High-level API end-to-end (ADR 0018): KlvStream reads a .ts, we edit a typed
// value on each Message, KlvSink writes a new .ts; re-reading proves the edit
// persisted through decode -> encode -> mux -> demux.
// argv: <input.ts> <out.ts>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "misbklv/stream.hpp"

using namespace misbklv;

static int failures = 0;
static void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: api_test <in.ts> <out.ts>\n"); return 2; }
  constexpr auto kSensorLatitude = tags::Uas0601::SensorLatitude;  // named ST 0601 tag

  // --- read + edit + emit ---------------------------------------------------
  std::vector<double> orig;
  {
    KlvStream in(argv[1]);
    KlvSink out(std::string("file:") + argv[2]);
    for (Message& m : in) {
      auto lat = m.get<double>(kSensorLatitude);
      if (lat) {
        orig.push_back(*lat);
        check(static_cast<bool>(m.set(kSensorLatitude, Value{*lat + 1.0})), "set lat");
      }
      check(static_cast<bool>(out.emit(m)), "emit");
    }
    check(static_cast<bool>(out.close()), "close sink");
  }
  check(!orig.empty(), "read SensorLatitude from some frames");

  // --- re-read and verify the edit persisted --------------------------------
  std::vector<double> got;
  {
    KlvStream re(argv[2]);
    for (Message& m : re)
      if (auto lat = m.get<double>(kSensorLatitude)) got.push_back(*lat);
  }
  check(got.size() == orig.size(), "same frame count after round-trip");
  bool all_edited = got.size() == orig.size();
  for (std::size_t i = 0; i < got.size() && i < orig.size(); ++i)
    if (std::fabs(got[i] - (orig[i] + 1.0)) > 0.01) all_edited = false;
  check(all_edited, "each SensorLatitude is source + 1.0");

  std::printf("%s (%zu frames)\n", failures == 0 ? "API: all PASS" : "API: FAIL",
              got.size());
  return failures == 0 ? 0 : 1;
}
