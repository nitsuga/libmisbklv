# libklv — Progress

Current status. Volatile; rewrite each session. For the plan, see
[ROADMAP.md](./ROADMAP.md).

## Now

Phase 0 (Foundation) complete. At the Phase 1 decision point — about to work
through the foundational forks (build system, C++ standard, license, name),
then core architecture.

## Done

- Repo initialized: git + git-lfs (PDFs/`.mpg` tracked via LFS), `main` branch.
- Documentation structure: `docs/` (user), `context/` (OKF agent KB),
  `references/` (immutable standards), `data/` (samples), `planning/` (this).
- OKF bundle seeded in `context/` (index/log/CONVENTIONS + root `CLAUDE.md`).
- Standards ingested: ST 0107, 1201, 0601, 0903, 0604 (`type: Standard Reference`).
- Prior art ingested: klvdata, klvp, libmisb0601, gstklvplugin,
  akrutsinger/libklv (`type: Prior Art`).
- `data/` samples characterized (`type: Sample Data`): verified ST 0601 in
  MPEG-TS, `0x06`+`KLVA` signaling.
- Fork 1 decided (ADR [`0001`](../context/decisions/0001-build-system-and-cpp-standard.md),
  accepted): CMake ≥3.20 + **C++20 floor** + cross-compile (native x86_64,
  `aarch64` for Jetson), JetPack 6+ minimum.

## In progress

(none — fork 1 decided; fork 2 (license) next)

## Next

- Resolve foundational forks 1–3 (build system, C++ std, license, name).
- Then fork 4 (core architecture).

## Blockers / notes

- None.
