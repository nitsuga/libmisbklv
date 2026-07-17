# libmisbklv — Progress

Current status. Volatile; rewrite each session. For the plan, see
[ROADMAP.md](./ROADMAP.md).

## Now

Phase 0 (Foundation) done. Phase 1 (foundational decisions) complete — forks
1 (build system & C++ standard), 2 (license), 3 (name → `libmisbklv`), and
7 (ADR format) decided. Next: fork 4 (core architecture).

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
- Fork 2 decided (ADR [`0002`](../context/decisions/0002-license.md),
  accepted): **Apache-2.0** (permissive, patent grant); `LICENSE` added;
  per-file SPDX headers when code lands.
- Fork 3 decided (ADR [`0003`](../context/decisions/0003-project-name.md),
  accepted): renamed project `libklv` → **`libmisbklv`** (Option B); external
  `akrutsinger/libklv` references preserved.
- Fork 7 decided (ADR [`0004`](../context/decisions/0004-adr-format.md),
  accepted): ADR format/numbering formalized (Nygard-style `NNNN-slug`,
  sequential by creation order, `status` frontmatter, body sections); spec in
  [`CONVENTIONS`](../context/CONVENTIONS.md) § ADR format.

## In progress

(none — fork 4 (core architecture) next)

## Next

- Fork 4 (core architecture): KLV core data model; parser / item-registry
  split; data-driven tag registry.
- Then forks 5 (gstreamer/ffmpeg backends), 6 (0604 scope).

## Blockers / notes

- Repo directory still `~/workspaces/libklv` (optional `mv` to `libmisbklv`,
  separate from the name decision).
