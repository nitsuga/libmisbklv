---
type: Decision
title: Project license
decision_status: accepted
tags: [decision, license, permissive, apache-2.0, phase-1]
generated:
  by: claude/opus-5
  at: 2026-07-17T16:30:00Z
sources:
  - resource: https://www.apache.org/licenses/LICENSE-2.0
    title: Apache License 2.0
fork: 2
---

# Context

Fork 2 of [`../../planning/ROADMAP.md`](../../planning/ROADMAP.md). Pick the
license before code lands. Clean-room implementation — no code lifted from
prior art. Prior-art license landscape: [klvdata](../prior-art-klvdata.md)
(MIT), [gstklvplugin](../prior-art-gstklvplugin.md) (AGPL-3.0), three others
unlicensed. libmisbklv is a *library* meant to be embedded by host apps (including
possibly commercial/UAS stacks), and integrates with gstreamer (LGPL) and
ffmpeg (LGPL/GPL).

# Decision

**Apache-2.0.** Permissive, with an explicit patent grant (relevant in
codec/media-adjacent territory — protects contributors and adopters), the
modern C++ default, and compatible with linking the LGPL gstreamer / ffmpeg.
A `LICENSE` file (canonical Apache-2.0 text) sits at the repo root; per-file
`SPDX-License-Identifier: Apache-2.0` headers are added to source files as
they land.

# Alternatives considered

- **MIT** — permissive, simpler/shorter, no patent grant; the runner-up.
  Rejected in favor of Apache-2.0's patent grant as the more defensible
  default for a library with potential commercial embedding.
- **LGPL-2.1 / 3.0** — weak copyleft; **rejected** (user prefers permissive;
  no requirement that lib modifications stay open).
- **AGPL-3.0** — strong copyleft incl. network use; **rejected** (would block
  embedding in closed UAS apps; gstklvplugin's choice but unsuitable for us).
- **GPL** — viral; **rejected**.

# Consequences

- No copyleft: anyone may embed libmisbklv in proprietary / closed-source apps.
  We forgo the lever of forcing lib-level modifications back into the open.
- Apache-2.0's patent grant applies to contributors and users.
- Compatible with linking LGPL gstreamer / ffmpeg (permissive can comply with
  LGPL by allowing relinking).
- Follow-up: per-file SPDX headers on source files (when code lands).

# Assumptions / open questions

- **Resolved:** permissive (user preference); Apache-2.0 over MIT for the
  patent grant.
- **Open (non-blocking):** per-file SPDX header style — settle when the first
  source files land.

# Citations

[1] [`klvdata`](../prior-art-klvdata.md) — MIT (permissive prior art).
[2] [`gstklvplugin`](../prior-art-gstklvplugin.md) — AGPL-3.0 (copyleft prior
    art; not adopted).
[3] Apache License 2.0 — <https://www.apache.org/licenses/LICENSE-2.0>.
