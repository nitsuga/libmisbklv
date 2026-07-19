---
type: Decision
title: Backend as an optional dependency
status: accepted
tags: [decision, backend, build, cmake, gstreamer, phase-3]
timestamp: 2026-07-19T01:00:00Z
fork: 14
---

# Context

Fork 14 (backend F-D). The KLV core ([`0005`](./0005-klv-core-data-model.md))
must stay dependency-free (embedding / Jetson / closed-app friendly,
[`0006`](./0006-tag-registry.md)), while the gstreamer backend
([`0013`](./0013-media-backend-interface.md)) links a heavy runtime. How should
the build keep gstreamer out of the core yet ship the backend when available?

# Decision

**A separate CMake target, `misbklv-gst`**, holding only the gstreamer
implementation (`src/gst/gst_backend.cpp`); it links `misbklv` + gstreamer. The
core `misbklv` library never references gstreamer. An `option(MISBKLV_GSTREAMER
ON)` gates it, and it builds only when `pkg_check_modules(gstreamer-1.0
gstreamer-app-1.0)` succeeds — otherwise it is silently skipped and the core
still builds. The interface / factory headers (`backend.hpp`, `mock_backend.hpp`,
`gst_backend.hpp`) are gstreamer-free, so consumers include them without the
dependency; only linking `misbklv-gst` pulls gstreamer in.

# Alternatives considered

- **One library, gstreamer compiled in conditionally** — would make the core
  target's ABI/deps depend on a flag and risk leaking gstreamer into core
  consumers. Rejected: a separate target is a cleaner seam.
- **Hard gstreamer dependency** — rejected: breaks the dependency-free core.
- **Runtime plugin (dlopen) backend** — over-engineered for v1; the interface
  ([`0013`](./0013-media-backend-interface.md)) already allows swapping backends
  at the source level (mock, future ffmpeg).

# Consequences

- `misbklv` (core) has zero media deps; `misbklv-gst` is opt-in.
- CI installs the gstreamer dev libs + plugins for the backend build/tests; the
  core build stays light (the `.ts` regression already needs ffmpeg only).
- `misbklv-gst` is not yet installed/exported (backend is WIP through B2+); add
  it to the export set when the backend stabilizes.

# Citations

[1] [`0013`](./0013-media-backend-interface.md) — the interface `misbklv-gst`
    implements.
[2] [`0006`](./0006-tag-registry.md) — dependency-free core rationale.
[3] [`backend-scope`](../backend-scope.md) — F-D.
