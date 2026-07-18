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
* **Decision (proposed)**: Fork 3 →
  [`0003-project-name`](./decisions/0003-project-name.md) (proposed): resolve
  the `akrutsinger/libklv` namesake collision. Propose keep `libklv` +
  disambiguate (collision is with an inactive stub); alternative rename now
  (cheap — no code yet, docs-only find-replace; candidates `libmisbklv` /
  `misbklv` / `klvio` / `klvpp`). Defer rejected (renaming cheapest now).
  Awaiting pick. ROADMAP fork 3 → PROPOSED.
* **Decision (accepted)**: [`0003`](./decisions/0003-project-name.md) →
  accepted. Renamed the project `libklv` → **`libmisbklv`** (Option B) to
  eliminate the `akrutsinger/libklv` collision while rename is free (no code
  yet). `libklv_cpp` rejected (implies a port; C-ABI-incompatible). Doc
  self-refs renamed; external `akrutsinger/libklv` refs preserved. ROADMAP
  fork 3 → DECIDED. Repo dir rename left as a separate optional step.
* **Decision (proposed)**: Fork 7 →
  [`0004-adr-format`](./decisions/0004-adr-format.md) (proposed): formalize the
  de-facto ADR format (Nygard-style `NNNN-slug.md`, sequential by creation
  order — fork 7 = ADR 0004; frontmatter + body sections; register).
  Alternatives rejected: MADR (overhead), ad-hoc, external-tool. Full spec in
  [`CONVENTIONS`](./CONVENTIONS.md) § ADR format. Awaiting accept. ROADMAP
  fork 7 → PROPOSED.
* **Decision (accepted)**: [`0004`](./decisions/0004-adr-format.md) → accepted.
  ADR format formalized: Nygard-style `NNNN-slug.md`, sequential by creation
  order (fork 7 = ADR 0004), `status` frontmatter, `# Decision` heading always,
  body Context/Decision/Alternatives/Consequences/Assumptions/Citations,
  register in `decisions/index.md`. Spec lives in [`CONVENTIONS`](./CONVENTIONS.md)
  § ADR format. ROADMAP fork 7 → DECIDED. **Phase 1 (foundational decisions)
  complete.**
* **Decision (proposed)**: Fork 4 (split) → three proposed ADRs —
  [`0005`](./decisions/0005-klv-core-data-model.md) (KLV core data model,
  hybrid: tag+length+raw bytes + registry-driven typed view),
  [`0006`](./decisions/0006-tag-registry.md) (compiled-in `constexpr`,
  build-time codegen), [`0007`](./decisions/0007-error-and-c-abi.md) (local
  `Result<T>`; C ABI deferred). Four-layer architecture agreed. ROADMAP fork 4
  → PROPOSED. Awaiting accept.
* **Decision (accepted)**: Fork 4 —
  [`0005`](./decisions/0005-klv-core-data-model.md)/[`0006`](./decisions/0006-tag-registry.md)/[`0007`](./decisions/0007-error-and-c-abi.md)
  all accepted. Core architecture locked: hybrid data model (tag+len+raw
  bytes + registry-driven typed view, `std::span` zero-copy); compiled-in
  `constexpr` tag registry (build-time codegen); local `Result<T>` (no
  exceptions for routine errors); C ABI deferred (clean C++ core first).
  ROADMAP fork 4 → DECIDED. **Phase 2 (core architecture) design complete.**
* **Decision (proposed)**: Fork 5 →
  [`0008-media-backend-gstreamer`](./decisions/0008-media-backend-gstreamer.md)
  (proposed): single backend **gstreamer for v1** (library-style; `MediaBackend`
  interface kept; extract + insert incl. real-time via `appsrc` flow control;
  **PMT-rewrite component required** for `KLVA` insertion signaling — porting
  gstklvplugin's `tspmtrewrite`; ffmpeg deferred/optional). Project goal narrows
  from "gstreamer or ffmpeg" to "gstreamer (v1)". Awaiting accept. ROADMAP fork 5
  → PROPOSED.
* **Decision (accepted)**: [`0008`](./decisions/0008-media-backend-gstreamer.md)
  → accepted. Single **gstreamer backend for v1** (library-style;
  `MediaBackend` interface kept). Extract + insert incl. **real-time async
  (`0x06`)** via `appsrc` flow control. **PMT-rewrite = in-library gstreamer
  element** (option (c), porting gstklvplugin's `tspmtrewrite`; not a shipped
  plugin). Sync per-frame (`0x15`) deferred. ffmpeg deferred/optional. Project
  goal narrowed to "gstreamer (v1)"; `CLAUDE.md`/`README` reconciled. ROADMAP
  fork 5 → DECIDED.
* **Decision (deferred)**: Fork 6 →
  [`0009-st0604-deferred`](./decisions/0009-st0604-deferred.md) (deferred): ST
  0604 ES-timestamp layer deferred from v1 (secondary target; separate
  video-ES layer; NAL/SEI work comparable to the PMT-rewrite + same
  per-frame-sync shape as deferred sync-KLV; 0601 item 2 covers common
  correlation). v1 = 0601/0903 KLV via gstreamer. `CLAUDE.md`/`README`
  reconciled. ROADMAP fork 6 → DEFERRED. **Design complete — all forks
  resolved.**
* **Spike (Phase 3)**: Extracted the KLV elementary stream from
  [`data-samples`](./data-samples.md) `Day Flight.mpg` (`ffmpeg -map 0:1 -c copy
  -f data`; KLV PID `0x1F1`) and walked the first ST 0601 packet end-to-end
  (throwaway parser, not library code) to ground the pending descriptor-schema /
  encode ADRs in real bytes. Verified: UL key `060e2b34…`, BER long-form length
  `0x8191` (145 B value / 163 B packet), Item 2 first / Item 1 last (mandatory
  ordering), Precision Time Stamp = 2009-06-17T16:53:05Z, sensor lat/lon =
  54.68°, −110.17°, and the **16-bit BCC checksum recomputed and matched
  (`0x1C5F`)** — the encode-path checksum invariant is implementable as
  specified.
* **Lint (correction)**: Spike falsified a claim in [`st0601`](./st0601.md) —
  legacy core numeric items (lat/lon/angles, tags 5–15) use a **linear LDS map**
  (explicit int→float range; Item 13 = `int32` `-((2^31)-1)..(2^31)-1` → ±90°,
  `0x80000000`="Reserved"; §8.13), **not** IMAPB. IMAPB applies to the *extended*
  items (tags ~90+). Corrected `st0601` § Encoding to discriminate the two
  mapping kinds — the central input to the descriptor schema (a per-item
  mapping-kind + params field).
* **Decision (proposed)**: Fork 8 →
  [`0010-registry-descriptor-schema`](./decisions/0010-registry-descriptor-schema.md)
  (proposed): completes the descriptor field set [`0006`](./decisions/0006-tag-registry.md)
  punted. Flat `constexpr` `ItemDescriptor` (tag, name, `ValueKind` discriminator,
  `LengthSpec`, `MappingParams`, inline `SpecialValue[]`, `childRegistry`, flags)
  in per-registry tables (`TagEncoding` BER-OID vs 1-byte-UINT; UL key). `ValueKind`
  pins the [`0005`](./decisions/0005-klv-core-data-model.md) typed-view variant;
  codecs are a small shared set parameterized by the descriptor, not per-item.
  Authoring source-of-truth format + codegen tool left as a 0006 follow-on.
  ROADMAP fork 8 → PROPOSED.
* **Decision (accepted)**: [`0010`](./decisions/0010-registry-descriptor-schema.md)
  → accepted. Descriptor schema locked; unblocks the typed view, the shared codec
  set, and codegen. ROADMAP fork 8 → DECIDED.
* **Decision (proposed)**: Fork 9 →
  [`0011-encode-model`](./decisions/0011-encode-model.md) (proposed): the write
  half. **Owned builder** (coexists with the borrow-by-default read model),
  **bottom-up assembly** (a value is serialized before its BER length — no
  back-patching; nested sets/packs built into child buffers), `finalize()`
  validates mandatory items + emits Item 2 first / Item 1 checksum last
  (spike-verified BCC), returns an owned buffer moved to `appsrc`
  ([`0008`](./decisions/0008-media-backend-gstreamer.md)). Adds `EncodeError`
  variants to [`0007`](./decisions/0007-error-and-c-abi.md); reuses 0010's
  `MappingParams` for forward mapping. Draws the read-borrows/write-owns
  ownership boundary. Acceptance gate = byte-exact round-trip of the spike packet.
  ROADMAP fork 9 → PROPOSED.
* **Decision (accepted)**: [`0011`](./decisions/0011-encode-model.md) → accepted.
  Encode model locked: owned builder, bottom-up assembly, `finalize()` mandatory/
  ordering/checksum emission, owned-buffer handoff, `EncodeError` variants.
  ROADMAP fork 9 → DECIDED. **All forks resolved; the design backlog is clear —
  Phase 3 implementation (milestone 1: byte-exact round-trip) is unblocked.**
* **Decision (proposed)**: Fork 10 →
  [`0012-registry-codegen`](./decisions/0012-registry-codegen.md) (proposed): the
  [`0006`](./decisions/0006-tag-registry.md) codegen follow-on. **TOML**
  source-of-truth per registry (array-of-tables; per-item standard citation in
  comments) + a **Python generator** (`tools/gen_registry.py`, stdlib `tomllib`,
  validates tag-uniqueness / mapping-params / childRegistry / special-fit) emitting
  the [`0010`](./decisions/0010-registry-descriptor-schema.md) `constexpr` tables.
  **Generated C++ committed** (no Python build dep for consumers; reviewable),
  with a `regenerate-registry` target + CI drift check. JSON/YAML/hand-C++/
  build-time-gen rejected. ROADMAP fork 10 → PROPOSED.
* **Decision (accepted)**: [`0012`](./decisions/0012-registry-codegen.md) →
  accepted. Registry codegen locked: TOML source + Python generator + committed
  generated C++ + drift check. ROADMAP fork 10 → DECIDED. **All forks resolved.**
* **Milestone 1 (implementation)**: byte-exact round-trip landed. Real C++
  header-only core (`include/misbklv/`: `ber`/`types`/`codec`/`packet`/`builder`,
  `Result<T>`) per ADRs 0005/0007/0010/0011; TOML→`constexpr` generator
  (`tools/gen_registry.py`, `registry/uas0601.toml` →
  `src/registry/uas0601_tables.generated.hpp`) per ADR 0012; CMake + CTest per
  ADR 0001. `roundtrip_test` decodes `Day Flight.mpg`'s first packet, re-encodes
  (typed codecs for registered items, raw for the rest), recomputes the checksum,
  and reproduces all 163 bytes identically. Validates the descriptor schema +
  encode model against real bytes. Generator output byte-identical under `tomli`
  and the embedded fallback reader (drift check). (Detail in PROGRESS.)
