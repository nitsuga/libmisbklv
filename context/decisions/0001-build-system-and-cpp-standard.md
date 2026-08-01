---
type: Decision
title: Build system & C++ standard
decision_status: accepted
tags: [decision, build, cmake, cpp-standard, cpp20, jetson, multi-arch, cross-compile, phase-1]
generated:
  by: claude/opus-5
  at: 2026-07-17T15:30:00Z
sources:
  - resource: https://developer.nvidia.com/embedded/jetpack-sdk-515
    title: NVIDIA JetPack 5.1.5
  - resource: https://developer.nvidia.com/embedded/jetpack-sdk-60
    title: NVIDIA JetPack 6.0
fork: 1
---

# Context

Fork 1 of [`../../planning/ROADMAP.md`](../../planning/ROADMAP.md). Foundational
and expensive to change; drives every later module. Prior art:
[klvp](../prior-art-klvp.md) (CMake + vcpkg), [gstklvplugin](../prior-art-gstklvplugin.md)
(Meson + CMake). A constraint surfaced in deliberation: the library may run on
**NVIDIA Jetson (Orin or newer)**. Resolved: **JetPack 6+ is the minimum
target**, and builds for Jetson are **cross-compiled only** (no native builds
on a Jetson). Multi-arch (x86_64 native + aarch64 cross) is in scope.

# Jetson / toolchain constraint (resolved)

- Minimum target runtime: **JetPack 6** (Ubuntu 22.04, libstdc++ from GCC
  11.4). JetPack 5 (GCC 9.3) is **not** supported.
- **No native Jetson builds.** Cross-compile from an x86_64 host to aarch64
  using a cross-toolchain we control; never rely on the Jetson's stock
  compiler.
- `std::span` needs GCC 10+ headers; our cross-toolchain (≥ GCC 11) provides
  them, and `span` is a header-only template → runtime-safe on JetPack 6's
  libstdc++ 11. So a C++20 `std::span`-based parser API is viable.
- Orin's GPU (NVENC/NVDEC, GStreamer `nvv4l2` plugins) serves the host app's
  video path, not libmisbklv core — KLV extraction is CPU `tsdemux`. The GPU
  doesn't constrain us, but we must coexist with NVIDIA's GStreamer stack
  (relevant to fork 5).

# Decision

- **Build system: CMake** ≥ 3.20. Multi-arch via cross-compile: native x86_64
  builds for desktop; `aarch64-linux-gnu` cross-toolchain files for Jetson (no
  native aarch64/Jetson builds). CI matrix: x86_64 + aarch64. No arch-specific
  assumptions in code. `option()` toggles + `find_package` / pkg-config for the
  optional gstreamer/ffmpeg backends. Single build system; no Meson in v1.
- **C++ standard: C++20 floor** (`CMAKE_CXX_STANDARD 20`, required). Compiler:
  a conforming C++20 toolchain (GCC ≥ 10; we'll use ≥ 11 host/cross). Use
  `std::span` for byte views over the KLV buffer (zero-copy parser API),
  `concepts` for the backend interface, and `std::bit_cast` where it helps.
- **C++23 deferred.** `std::expected` would be nice for parser error returns
  but wants libstdc++ 12; a local `Result<T>` bridges at C++20. Bump to C++23
  later if desired (viable now that the toolchain is ours to choose).

# Alternatives considered

## Build system

- **Meson** — cleaner, GStreamer-native; rejected as primary (consumers expect
  CMake + `find_package`). Revisit as a secondary generator if gstreamer
  integration proves painful. Bazel/Buck2 — overkill. Hand-rolled Make — too
  manual for optional deps + multi-backend.

## C++ standard

- **C++17** — considered while JetPack-5-native (GCC 9, no `std::span`) was in
  scope; **rejected** once the floor moved to JetPack 6+ / cross-compile (span
  available). Would forgo span, concepts, ranges, bit_cast for no remaining
  benefit.
- **C++20 (chosen)** — span is the load-bearing win for the parser; concepts
  tidy the backend interface; all at negligible toolchain cost.
- **C++23** — `std::expected`; deferred (polyfill `Result<T>`). Bump later if
  wanted.

# Consequences

- CMake config exported for consumers; backends compile-selected via options →
  ABI differs per backend combo (document).
- Cross-compile is the only path to aarch64/Jetson: provide toolchain files +
  a JetPack-6 aarch64 sysroot; CI matrix x86_64 + aarch64. No native Jetson
  builds (saves Jetson resources; matches the "cross-compile only" choice).
- C++20 floor sets the minimum toolchain; a `std::span`-based parser API
  commits us to span ergonomics throughout.
- Coexistence with NVIDIA's GStreamer stack noted for the gstreamer backend
  (fork 5).

# Assumptions / open questions

- **Resolved:** JetPack 6+ minimum; cross-compile only (no native Jetson). These
  removed the C++17 rationale and selected C++20.
- **Open (non-blocking):** bump to C++23 for `std::expected` later? Viable now
  (toolchain is ours); decide near parser error-handling design.
- Windows/macOS: not v1; CMake keeps the door open.
- ADR filename convention `NNNN-slug.md` provisional (fork 7 formalizes).

# Citations

[1] NVIDIA JetPack 5.1.5 — Ubuntu 20.04, GCC 9.3 (the version lacking `std::span`;
    **not** supported). <https://developer.nvidia.com/embedded/jetpack-sdk-515>
[2] NVIDIA JetPack 6.0 — Ubuntu 22.04, GCC 11.4 (minimum supported target).
    <https://developer.nvidia.com/embedded/jetpack-sdk-60>
[3] `std::span` in libstdc++ requires GCC 10 (`-std=c++20` + concepts);
    header-only template, runtime-safe on libstdc++ ≥ 10. `__cpp_lib_span` = 202002L.
