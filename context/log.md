# Knowledge Bundle Log

## 2026-07-17

* **Initialization**: Established OKF v0.1 bundle — `index.md`, `log.md`, `CONVENTIONS.md`.
* **Schema**: Seeded open `type` vocabulary (Standard Reference, Encoding Rule, KLV Item, Prior Art, Decision, Component) in `CONVENTIONS.md`.
* **Pointer**: Filled root `CLAUDE.md` as the auto-load entry into this bundle.
* **Ingest**: Added five `type: Standard Reference` concepts —
  [`st0107`](./st0107.md) (KLV core), [`st1201`](./st1201.md) (IMAP mapping),
  [`st0601`](./st0601.md) (UAS Datalink LS), [`st0903`](./st0903.md) (VMTI),
  [`st0604`](./st0604.md) (ES timestamps). Cross-linked dependencies;
  populated `index.md` Standard References section.
* **Ingest**: Added five `type: Prior Art` concepts —
  [`klvdata`](./prior-art-klvdata.md), [`klvp`](./prior-art-klvp.md),
  [`libmisb0601`](./prior-art-libmisb0601.md),
  [`gstklvplugin`](./prior-art-gstklvplugin.md),
  [`akrutsinger/libklv`](./prior-art-libklv-akrutsinger.md).
  Key takeaways: parser-vs-item-DB split (klvp), INI-driven tag registry +
  PMT `0x06`+`KLVA` signaling + element decomposition (gstklvplugin), the
  ffmpeg `-map data-re` demux idiom on our exact sample (klvdata). Flagged
  the `akrutsinger/libklv` name collision for a future Decision.
  Populated `index.md` Prior Art section.
* **Ingest**: Added [`data-samples`](./data-samples.md) (`type: Sample Data`) —
  characterized `Day Flight.mpg` / `Night Flight IR.mpg` via `ffprobe` + a TS/PMT/UL
  scan. Verified: ST 0601 UL key, BER long-form length, tag-2-first packet;
  KLV PID `0x1F1`, **`stream_type=0x06` + `KLVA`** (not `0x15`) — validates the
  gstklvplugin mux-signaling note against real samples.
* **Lint**: Promoted `Sample Data` from anticipated to current type in
  [`CONVENTIONS`](./CONVENTIONS.md) (first use). Added a `Sample Data` section
  to `index.md`.
* **Structure**: Added [`../planning/`](../planning/) (ROADMAP.md, PROGRESS.md)
  for transient planning/progress — distinct from this KB (evergreen) and
  `../docs/` (user-facing). Added a `# Decisions` section to
  [`CONVENTIONS`](./CONVENTIONS.md) with the `status:` vocabulary
  (proposed/accepted/superseded/deferred) and the open-fork → Decision
  lifecycle. Root `CLAUDE.md` now points at `planning/`.
* **Structure**: Created [`./decisions/`](./decisions/) subdir (with an ADR
  register, [`./decisions/index.md`](./decisions/index.md)) to group Decision
  concepts; root `index.md` Decisions section now points to it. Added a
  `# Subdirectories` policy to [`CONVENTIONS`](./CONVENTIONS.md): types stay
  flat by default, split only when noisy (~10+) or sub-structured (likely
  future split: `items/` for per-item 0601 concepts, deferred). Fork 7 in
  [`../planning/ROADMAP`](../planning/ROADMAP.md) narrowed to ADR
  format/numbering (location settled).
* **Process**: Added a `## Planning hygiene` directive to root `CLAUDE.md` —
  on a significant decision, write/update the ADR in `context/decisions/` and
  refresh ROADMAP (fork status + link) + PROGRESS; on implementing a
  significant change, refresh PROGRESS (and ROADMAP if a fork moves); routine
  ingests/lint stay in `log.md`. Ensures planning stays non-ephemeral.
* **Decision (proposed)**: Fork 1 →
  [`0001-build-system-and-cpp-standard`](./decisions/0001-build-system-and-cpp-standard.md)
  (proposed): CMake ≥3.20 + C++20 floor. Awaiting accept; flagged the
  embedded-toolchain assumption as the thing most likely to flip it to C++17.
  ROADMAP fork 1 → PROPOSED; ADR filename convention `NNNN-slug.md` adopted
  provisionally (fork 7 to formalize).
* **Decision (revised)**: Revised
  [`0001`](./decisions/0001-build-system-and-cpp-standard.md) on the Jetson
  constraint: JetPack 5 (Orin) ships GCC 9.3, which lacks `std::span` (needs
  GCC 10), so a C++20 span-based API won't build natively on stock JetPack 5.
  Floor revised C++20 → **C++17**; added multi-arch/cross-compile (x86_64 +
  aarch64) as a first-class CMake concern; byte views via `string_view` + a
  local view type. GPU noted as host-app/video-path, not core. Gate to
  acceptance: is JetPack-5-native required? (recommend yes → C++17).
* **Decision (accepted)**: [`0001`](./decisions/0001-build-system-and-cpp-standard.md)
  → accepted. User resolved the gate: **JetPack 6+ minimum, cross-compile only
  (no native Jetson builds)**. This removed the C++17 rationale → floor is
  **C++20** (`std::span` viable: header-only, cross-toolchain ≥ GCC 11). Build
  = CMake ≥3.20, multi-arch (native x86_64 + `aarch64` cross), C++23 deferred
  (local `Result<T>`). ROADMAP fork 1 → DECIDED.
* **Decision (proposed)**: Fork 2 →
  [`0002-license`](./decisions/0002-license.md) (proposed): permissive license
  — Apache-2.0 recommended (patent grant; codec/media-adjacent) with MIT as
  the lighter alternative. LGPL/AGPL/GPL rejected (user prefers permissive;
  AGPL blocks closed embedding). Awaiting SPDX pick (Apache-2.0 vs MIT) to
  accept. ROADMAP fork 2 → PROPOSED.
* **Decision (accepted)**: [`0002`](./decisions/0002-license.md) → accepted.
  License = **Apache-2.0** (permissive, patent grant). Canonical `LICENSE`
  added at repo root; per-file `SPDX-License-Identifier: Apache-2.0` headers
  when code lands. ROADMAP fork 2 → DECIDED.
