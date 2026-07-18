# libmisbklv — Progress

Current status. Volatile; rewrite each session. For the plan, see
[ROADMAP.md](./ROADMAP.md).

## Now

Phase 0 done. Phase 1 complete. Phase 2 (core architecture + media backend)
decided — forks 4 (0005/0006/0007), 5 (0008) accepted; fork 6 (0604) deferred.
Phase 3 (implementation) **started with an extraction spike** on `Day
Flight.mpg`, which validated the read path (BER walk, timestamp, lat/lon,
checksum) and surfaced two design gaps the "all forks resolved" status missed.
All design forks (1–10) resolved. **Milestones 1–4 done and passing** (5 CTest
cases green). M1 = first-packet round-trip. M2 = full-stream round-trip of both
0601 samples (Day 6/977 B, Night 18/2916 B), full 25-tag registry. M3 = 0903
nesting (Item 74 VMTI, recursive `childRegistry` routing + parse/build). M4 =
**real ST 1201 IMAPB codec** — validated against the standards' own worked
examples (ST 1201 Table 7; ST 0903 §10.1.11 FoV) *and* exercised end-to-end via
VMTI FoV items in the nested fixture. Next: **M5 — VTarget Series / Array-Series
packs** (0903 Item 101), then split the header-only core into a real lib target +
CI drift check, then the gstreamer backend.

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
- Fork 8 decided (ADR [`0010`](../context/decisions/0010-registry-descriptor-schema.md),
  accepted): registry descriptor schema — flat `constexpr` `ItemDescriptor`
  (tag, name, `ValueKind` discriminator {raw-int, linear-LDS, IMAPB, utf8,
  bytes, nested-LS, pack}, `LengthSpec`, `MappingParams`, inline `SpecialValue[]`,
  `childRegistry`, flags) in per-registry tables (`TagEncoding` BER-OID vs
  1-byte-UINT; UL key). Pins the [`0005`](../context/decisions/0005-klv-core-data-model.md)
  typed-view variant; codecs are a shared set parameterized by the descriptor.
  Authoring source format + codegen tool remain a [`0006`](../context/decisions/0006-tag-registry.md)
  follow-on.
- Fork 9 decided (ADR [`0011`](../context/decisions/0011-encode-model.md),
  accepted): encode / serialization model — owned builder (coexists with the
  borrow read model), bottom-up assembly (value before BER length, no
  back-patching), `finalize()` validates mandatory items + emits Item 2 first /
  Item 1 checksum last (spike-verified BCC), owned-buffer handoff to `appsrc`.
  Adds `EncodeError` variants to [`0007`](../context/decisions/0007-error-and-c-abi.md);
  reuses 0010's `MappingParams` for forward mapping. Draws the read-borrows /
  write-owns ownership boundary.
- Fork 10 decided (ADR [`0012`](../context/decisions/0012-registry-codegen.md),
  accepted): registry codegen — **TOML** source-of-truth per registry + a
  **Python generator** (`tools/gen_registry.py`, stdlib `tomllib`, validates)
  emitting the 0010 `constexpr` tables; **generated C++ committed** (no Python
  build dep for consumers) + `regenerate-registry` target + CI drift check.
  JSON/YAML/hand-C++/build-time-gen rejected. **All forks resolved.**
- **Extraction spike** — `ffmpeg` demux of `Day Flight.mpg` KLV PID (`0x1F1`) +
  a throwaway parser walked the first 0601 packet; grounded ADRs 0010/0011 and
  corrected the [`st0601`](../context/st0601.md) mapping claim.
- **Milestone 1 — byte-exact round-trip (Phase 3)** — real C++ (`include/misbklv/`
  header-only core: `ber`, `types`, `codec`, `packet`, `builder`; `Result<T>`),
  the TOML→constexpr generator (`tools/gen_registry.py` + `registry/uas0601.toml`
  → committed `src/registry/uas0601_tables.generated.hpp`), CMake + CTest, and a
  163-byte fixture. `roundtrip_test` decodes → re-encodes → asserts identical
  bytes: **PASS**. Generator verified byte-identical under `tomli` and the
  embedded fallback reader (ADR 0012 drift check).
- **Milestone 2 — full-stream round-trip (Phase 3)** — `stream_roundtrip_test`
  walks every packet in both samples and reconstructs the whole stream
  byte-identically (Day 6/6, Night 18/18). Registry expanded to the full 25-tag
  sample set (26 descriptors, §8-sourced ranges); the encoder is now
  length-parameterized so items reserialize at their **source width** (e.g.
  Item 22 Target Width at 4 B — non-canonical vs 0601.19's uint16); mandatory
  enforcement made opt-out on `finalize()` for faithful reserialization. New
  fixtures: `dayflight.klv`, `nightflight_ir.klv`.
- **Milestone 3 — 0903 nesting (Phase 3)** — restored `RegistryId` +
  `child` to the descriptor (ADR 0010) and a `registry_for()` resolver; added a
  second registry (`vmti0903.toml` → generated table), recursive `parse_items`
  (bare TLV walk for nested sets), and `LocalSetBuilder::serialize_items()` (nested
  value, no key/checksum). A hand-authored fixture (`vmti_nested.klv`, built by
  `make_vmti_fixture.py` from the standard) — an 0601 packet nesting a VMTI LS in
  Item 74 — round-trips byte-exact inner and outer. Validates childRegistry
  routing + recursive parse/build. Scalar-only: VTarget Series deferred to M5.
- **Milestone 4 — ST 1201 IMAPB codec (Phase 3)** — `ValueKind::IMAPB` now uses
  the real ST 1201 Starting-Point-B mapping (`bPow`/`dPow`/`sF`/`sR`/`Zoffset`),
  split from the linear path. `imapb_test` checks it against the standards'
  published values — ST 1201 §10 Table 7 (IMAPB(-900,19000) L=3: 10.0↔0x038E00)
  and ST 0903 §10.1.11 (IMAPB(0,180) L=2: 12.5°↔0x0640) — plus a round-trip
  sweep. VMTI FoV items 11/12 added to the registry + nested fixture, decoding
  12.5°/10.0° and re-encoding byte-exact.

## In progress

- (nothing active — Milestones 1–4 landed; picking the next target below.)

## Next

- **M5 — VTarget Series / packs (0903 Item 101)**: Array/Series homogeneous VLPs
  (0903 §9.1.2/3) + the VTarget Pack (BER-OID targetId + nested VTarget LS) — the
  recursive-core case M3/M4 deferred. Needs a Series/pack representation in the
  data model + builder and a hand-authored fixture.
- **Then** split the header-only core into a real library target (`.cpp`), wire
  the CI drift check (ADR 0012) into an actual CI config, and start the gstreamer
  backend incl. the in-library PMT-rewrite element
  ([`0008`](../context/decisions/0008-media-backend-gstreamer.md)).
- **Known gaps** (need vectors/data): Report-on-Change trimmed packets (samples
  are all full); a real VMTI stream (M3 uses a hand-authored fixture);
  multi-byte BER-OID tags (≥128, e.g. Item 143 MSID — none in samples yet).

## Blockers / notes

- Repo directory still `~/workspaces/libklv` (optional `mv` to `libmisbklv`,
  separate from the name decision).
