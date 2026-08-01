---
type: Decision
title: Project name — akrutsinger/libklv collision (resolved)
decision_status: accepted
tags: [decision, naming, libmisbklv, phase-1]
generated:
  by: claude/opus-5
  at: 2026-07-17T17:30:00Z
fork: 3
---

# Context

Fork 3 of [`../../planning/ROADMAP.md`](../../planning/ROADMAP.md). Our project
was named `libklv`, colliding with the existing GitHub repo
[`akrutsinger/libklv`](../prior-art-libklv-akrutsinger.md) — a C library for
ST 0601.9, no license, a 70-byte stub README, 4★, updated 2026-07-13 (recently
touched but inactive content).

**Key timing fact:** at decision time we had **no code — only docs**, so
renaming was a find-replace across docs (cheap); once code, CMake targets, and
package names land, renaming becomes expensive. Now was the cheapest time to
settle it.

# Decision

**Rename to `libmisbklv`.** Chose Option B (rename now) over keep +
disambiguate, to eliminate the collision forever while it's still free. The
project, CMake package, and library artifact are `libmisbklv` (CMake target
`misbklv` → `libmisbklv.a` / `libmisbklv.so`).

# Why this name

- Names the **domain** (MISB KLV — these *are* MISB standards); unique;
  language-agnostic.
- We will likely expose a **C ABI** (for gstreamer's C plugins, Python/ctypes,
  … — gstklvplugin is C), so a `_cpp` language suffix would mislead.
- Consistent with the ecosystem (`libmisb0601` already exists). The C++ signal
  is carried by the README and `CMakeLists.txt`, not the name.

# Alternatives considered

- **Option A — keep `libklv` + disambiguate** — rejected: leaves collision
  risk; we prefer zero risk while rename is free.
- **Defer** — rejected: rename is cheapest *now* (no code); deferring pushes
  cost into the expensive window.
- **`libklv_cpp`** — rejected: implies a *port* of someone else's `libklv`
  (derivative framing); `_` is non-idiomatic for CMake artifacts; misleading if
  a C ABI is added. `klvpp` / `libklvpp` were considered (fine, less
  descriptive) — `libmisbklv` preferred.

# Consequences

- Project, CMake package, and artifact are `libmisbklv`. Doc self-references
  renamed `libklv` → `libmisbklv`; **external `akrutsinger/libklv` references
  preserved** (that repo keeps its name).
- The repo *directory* `~/workspaces/libklv` is unchanged by this decision —
  an optional separate `mv` (doesn't affect the project name in docs).

# Assumptions / open questions

- **Resolved:** rename to `libmisbklv` (Option B).
- **Open (non-blocking):** repo directory rename — user's call, separate from
  the name decision.

# Citations

[1] [`prior-art-libklv-akrutsinger`](../prior-art-libklv-akrutsinger.md) — the
    namesake stub.
[2] [`0001`](./0001-build-system-and-cpp-standard.md) — sets the CMake
    package / artifact context.
