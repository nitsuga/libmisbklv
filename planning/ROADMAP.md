# libmisbklv — Roadmap

The plan: scope, sequencing, and open forks. Living doc — rewrite freely.
For "where are we right now" see [PROGRESS.md](./PROGRESS.md). Decisions
(resolved rationale) live in [`../context/`](../context/) as `type: Decision`
concepts; this file tracks them as a checklist.

## Scope (v1)

libmisbklv: a C++ library to read and write MISB KLV data — ST 0601 (UAS
Datalink LS), ST 0903 (VMTI), ST 0604 (ES-layer timestamps) — from/to MPEG-TS
containers via a configurable gstreamer or ffmpeg backend, file or stream.

Out of scope (v1, revisit): ST 0102 security, ST 0807 registry as a data
product, ST 1607 amend/segment beyond MSID passthrough.

## Phases

- **Phase 0 — Foundation** (done): repo + git/LFS, docs structure, OKF
  bundle, standards + prior-art + sample ingests.
- **Phase 1 — Foundational decisions**: build system, C++ standard, license,
  name collision.
- **Phase 2 — Core architecture**: KLV core data model; parser / item-registry
  split; data-driven tag registry.
- **Phase 3 — Media backends**: gstreamer + ffmpeg; MPEG-TS demux/mux;
  extract/insert KLV (file + stream), using `../data/` samples as round-trip
  targets.
- **Phase 4 — 0604 ES timestamps** (if in v1 scope): SEI / `user_data`
  injection/extraction, emulation-prevention stuff/de-stuff.
- **Phase 5 — Hardening**: tests, conformance, packaging, user docs.

## Open forks

Status legend: `OPEN` (undiscussed) · `PROPOSED` (Decision concept written,
`status: proposed`) · `DECIDED` (`status: accepted`) · `DEFERRED`.

| # | Fork | Status | Decision |
|---|------|--------|----------|
| 1 | Build system & C++ standard (CMake; C++17/20/23) | DECIDED | [0001](../context/decisions/0001-build-system-and-cpp-standard.md) |
| 2 | Project license | DECIDED | [0002](../context/decisions/0002-license.md) |
| 3 | `akrutsinger/libklv` name collision — renamed to `libmisbklv` | DECIDED | [0003](../context/decisions/0003-project-name.md) |
| 4 | Core architecture (data model / registry / error+ABI) | DECIDED | [0005](../context/decisions/0005-klv-core-data-model.md) [0006](../context/decisions/0006-tag-registry.md) [0007](../context/decisions/0007-error-and-c-abi.md) |
| 5 | gstreamer vs ffmpeg backends — shared interface; both in v1 or sequence | OPEN | — |
| 6 | 0604 ES-timestamp scope — v1 or deferred | OPEN | — |
| 7 | Decision records — ADR format & numbering | DECIDED | [0004](../context/decisions/0004-adr-format.md) |

When a fork is deliberated, write a `type: Decision` concept in `../context/`
(`status: proposed` → `accepted`), link it here, and update PROGRESS.
