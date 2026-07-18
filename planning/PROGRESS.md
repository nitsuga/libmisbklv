# libmisbklv — Progress

Current status. Volatile; rewrite each session. For the plan, see
[ROADMAP.md](./ROADMAP.md).

## Now

Phase 0 done. Phase 1 complete. Phase 2 (core architecture + media backend)
decided — forks 4 (0005/0006/0007), 5 (0008) accepted; fork 6 (0604) deferred.
**Design complete.** Phase 3 (implementation) next.

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
- Fork 4 decided (core architecture; ADRs
  [`0005`](../context/decisions/0005-klv-core-data-model.md),
  [`0006`](../context/decisions/0006-tag-registry.md),
  [`0007`](../context/decisions/0007-error-and-c-abi.md), all accepted):
  hybrid data model (tag+len+bytes + registry-driven typed view, `std::span`
  zero-copy); compiled-in `constexpr` tag registry (build-time codegen); local
  `Result<T>` (no exceptions for routine errors); C ABI deferred.
- Fork 5 decided (ADR [`0008`](../context/decisions/0008-media-backend-gstreamer.md),
  accepted): single gstreamer backend for v1 (library-style; `MediaBackend`
  interface kept); extract + insert incl. real-time (async `0x06`) via
  `appsrc`; PMT-rewrite as an in-library gstreamer element (porting
  gstklvplugin's `tspmtrewrite`); ffmpeg deferred. Project goal narrowed to
  "gstreamer (v1)".
- Fork 6 deferred (ADR [`0009`](../context/decisions/0009-st0604-deferred.md)):
  ST 0604 ES-timestamp layer deferred from v1 (secondary target; 0601 item 2
  timestamps cover the common correlation need). v1 = 0601/0903 KLV via
  gstreamer.

## In progress

(none — design complete; Phase 3 implementation next)

## Next

- Phase 3: implement against the locked design — KLV core
  ([`0005`](../context/decisions/0005-klv-core-data-model.md)), registry
  ([`0006`](../context/decisions/0006-tag-registry.md)), `Result<T>`
  ([`0007`](../context/decisions/0007-error-and-c-abi.md)), gstreamer backend
  incl. the in-library PMT-rewrite element
  ([`0008`](../context/decisions/0008-media-backend-gstreamer.md)). Round-trip
  targets: `data/Day Flight.mpg`, `data/Night Flight IR.mpg`.

## Blockers / notes

- Repo directory still `~/workspaces/libklv` (optional `mv` to `libmisbklv`,
  separate from the name decision).
