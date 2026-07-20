# libmisbklv

[![CI](https://github.com/nitsuga/libmisbklv/actions/workflows/ci.yml/badge.svg)](https://github.com/nitsuga/libmisbklv/actions/workflows/ci.yml)

C++20 library to read and write MISB KLV metadata — ST 0601 (UAS Datalink Local
Set) + ST 0903 (VMTI) — from/to MPEG-TS containers via
[GStreamer](https://gstreamer.freedesktop.org/) (file or stream; real-time
insertion via `appsrc`). ST 0604 (ES-layer timestamps) and an ffmpeg backend
are deferred — see [ADR 0008](context/decisions/0008-media-backend-gstreamer.md)
and [ADR 0009](context/decisions/0009-st0604-deferred.md).

## Features

- **ST 0601** UAS Datalink LS and **ST 0903** VMTI — embedded (Item 74),
  standalone, and VTarget Series — decode and byte-exact re-encode.
- **ST 1201 IMAPB** float↔integer mapping (incl. structural special values),
  cross-checked against the standards' vectors and jmisb.
- **MPEG-TS via GStreamer**: extract (`stream_type` 0x06 **and** 0x15, from a
  file or a live `udp:` / `srt:` source) and insert (file or live, clock-paced),
  all with stock GStreamer — no custom plugin.
- **gst-free file extraction**: pull KLV from a `.ts` buffer with zero
  dependencies (`extract_ts_klv`); GStreamer is only needed for live sources.
- **High-level API**: an owned, editable `Message` (typed `get<T>`/`set`,
  byte-exact `encode`) plus a `KlvStream` / `KlvSink` read-edit-write facade.

## Quick start

```cpp
#include "misbklv/stream.hpp"
using namespace misbklv;

KlvStream in("input.ts");            // a file, or "udp:127.0.0.1:5004" / "srt:..."
KlvSink   out("file:output.ts");
for (Message& m : in) {
  if (auto lat = m.get<double>(13))  // ST 0601 tag 13 = Sensor Latitude
    m.set(13, Value{*lat + 0.001});  // nudge ~100 m north
  out.emit(m);
}
out.close();
```

Full walkthrough (including the gstreamer-free path) in [`docs/api.md`](docs/api.md).

## Use in your project

```cmake
# Core only (Message, parse, codec) — no gstreamer dependency:
find_package(misbklv REQUIRED)
target_link_libraries(app PRIVATE misbklv::misbklv)

# ...or with the streaming facade (KlvStream / KlvSink), which needs gstreamer:
find_package(misbklv REQUIRED COMPONENTS gst)
target_link_libraries(app PRIVATE misbklv::gst)
```

## Build from source

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

Requirements: a **C++20** compiler (GCC ≥ 11) and **CMake ≥ 3.20**. **GStreamer
≥ 1.20** is optional — needed only for the streaming facade (`misbklv::gst`). A
few large test vectors live in [git-lfs](https://git-lfs.com/); run
`git lfs pull` to materialize them for the extraction tests.

## Status

The KLV core (ST 0601 + ST 0903) and the GStreamer media backend are implemented
and tested, and the library is installable via `find_package`. See
[`planning/ROADMAP.md`](planning/ROADMAP.md) and
[`planning/PROGRESS.md`](planning/PROGRESS.md) for the plan and current status,
and [`context/decisions/`](context/decisions/) for the architectural decisions
(ADRs).

## License

Apache-2.0 — see [`LICENSE`](LICENSE).
