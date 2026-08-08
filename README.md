# libmisbklv

[![CI](https://github.com/nitsuga/libmisbklv/actions/workflows/ci.yml/badge.svg)](https://github.com/nitsuga/libmisbklv/actions/workflows/ci.yml)

C++20 library to read and write MISB KLV metadata — ST 0601 (UAS Datalink Local
Set) + ST 0903 (VMTI) — from/to MPEG-TS containers via
[GStreamer](https://gstreamer.freedesktop.org/) (file or stream; real-time
insertion via `appsrc`). Video passthrough can generate ST 0604 Precision Time
Stamp SEI into the H.264 stream on request
([ADR 0024](context/decisions/0024-sei-generation-opt-in.md); off by default,
so passthrough video is byte-identical); the rest of
ST 0604 (ES-layer timestamp *reading*, H.265 Nano, Commercial time code) and an
ffmpeg backend are deferred — see
[ADR 0008](context/decisions/0008-media-backend-gstreamer.md) and
[ADR 0009](context/decisions/0009-st0604-deferred.md).

## Features

- **ST 0601** UAS Datalink LS and **ST 0903** VMTI — embedded (Item 74),
  standalone, and VTarget Series — decode and byte-exact re-encode.
- **ST 1201 IMAPB** float↔integer mapping (incl. structural special values),
  cross-checked against the standards' vectors and jmisb.
- **MPEG-TS via GStreamer**: extract `stream_type` 0x06 from a file or live
  `udp:` / `srt:` source, and insert to a file or live sink (clock-paced), all
  with stock GStreamer — no custom plugin.
- **Video passthrough on insert**: point the sink at a source file and its video
  elementary stream is re-muxed unchanged (parsed, never decoded) alongside your
  KLV — one call writes a TS with both a video PID and a KLV PID.
- **gst-free file extraction**: pull `stream_type` 0x06 **and** 0x15 KLV from a
  `.ts` buffer with zero dependencies (`extract_ts_klv`); GStreamer is only
  needed for live sources.
- **High-level API**: an owned, editable `Message` (typed `get<T>`/`set`,
  byte-exact `encode`) plus a `KlvStream` / `KlvSink` read-edit-write facade —
  read and write share one timeline, so editing a stream doesn't re-time it;
  terminal streaming errors are checked explicitly after iteration.

## Quick start

```cpp
#include "misbklv/stream.hpp"
using namespace misbklv;

KlvStream in("input.ts");            // a file, or "udp:127.0.0.1:5004" / "srt:..."
KlvSink   out("file:output.ts");
if (out.error()) return;               // open_insert failed
for (Message& m : in) {
  if (auto lat = m.get<double>(tags::Uas0601::SensorLatitude))
    m.set(tags::Uas0601::SensorLatitude, Value{*lat + 0.001});   // nudge ~100 m north
  if (!out.emit(m)) return;
}
if (in.error()) return;                // extraction or Message parse failed
if (!out.close()) return;
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

The build and tests use project-owned synthetic fixtures. The `data/` directory
is reserved for developer-provided media and is not required for a normal build.
The small generated fixtures are committed, so a normal build does not require
Python or network access.

### Requirements

- **CMake ≥ 3.20** and a **C++20** compiler (GCC ≥ 11). The core library
  (`misbklv::misbklv`) has no additional runtime or link dependencies.
- **GStreamer ≥ 1.20** — *optional*, only for the streaming facade
  (`misbklv::gst`). To **build** it you need the dev files for `gstreamer-1.0`
  and `gstreamer-app-1.0`; to **run** it (and the gstreamer tests) you also need
  the runtime plugins that provide the pipeline elements — MPEG-TS mux/demux and
  SRT are in *plugins-bad*, UDP in *plugins-good*, app/core in *plugins-base*.
  Without GStreamer the core still builds and its tests run; the facade and its
  tests are skipped.

### Ubuntu / Debian

```sh
# core build + tests
sudo apt-get install -y cmake g++

# ...plus the streaming facade (misbklv::gst): dev files + runtime plugins
sudo apt-get install -y \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
```

### Other Linux

Names vary by distro; you need a C++20 toolchain and CMake, and — for the facade
— the GStreamer 1.x dev files (`gstreamer-1.0`, `gstreamer-app-1.0`) plus
the base/good/bad runtime plugins. For example:

- **Fedora:** `gcc-c++ cmake gstreamer1-devel gstreamer1-plugins-base-devel gstreamer1-plugins-base gstreamer1-plugins-good gstreamer1-plugins-bad-free`
- **Arch:** `gcc cmake gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad`

Python 3.11+ is needed only to run the optional `regenerate-registry` and
`regenerate-synthetic-fixtures` targets; their committed outputs are checked for
drift in CI.

## Status

The KLV core (ST 0601 + ST 0903) and the GStreamer media backend are implemented
and tested, and the library is installable via `find_package`. See
[`planning/ROADMAP.md`](planning/ROADMAP.md) and
[`planning/PROGRESS.md`](planning/PROGRESS.md) for the plan and current status,
and [`context/decisions/`](context/decisions/) for the architectural decisions
(ADRs).

Agent working instructions live in [`AGENTS.md`](AGENTS.md) — the one canonical,
vendor-neutral copy. `CLAUDE.md` is a one-line `@AGENTS.md` import, since Claude
Code auto-loads that filename; supporting another agent means adding another
thin pointer, never a second copy of the rules.

## License

Apache-2.0 — see [`LICENSE`](LICENSE).
