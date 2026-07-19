# libmisbklv — Progress

Current status. Volatile; rewrite each session. For the plan, see
[ROADMAP.md](./ROADMAP.md).

## Now

Phase 0 done. Phase 1 complete. Phase 2 (core architecture + media backend)
decided — forks 4 (0005/0006/0007), 5 (0008) accepted; fork 6 (0604) deferred.
Phase 3 (implementation) **started with an extraction spike** on `Day
Flight.mpg`, which validated the read path (BER walk, timestamp, lat/lon,
checksum) and surfaced two design gaps the "all forks resolved" status missed.
All design forks (1–10) resolved. **Milestones 1–6 done and passing** (12 CTest
cases green): unit + hand-authored round-trips, jmisb/ST 1201 Annex A IMAP
cross-checks, jmisb 0601/0903 codec cross-checks, and **real-stream regression**
over `data/*.ts` (KLV extracted at build time via ffmpeg; 4 streams / ~4678
packets round-trip byte-exact). The core is a **compiled, installable library**
(`libmisbklv.a`, `find_package(misbklv)`) with **CI** (build/test/drift). M1 =
first-packet round-trip. M2 = full-stream round-trip of both
0601 samples. M3 = 0903 nesting (Item 74 VMTI, recursive `childRegistry`). M4 =
real ST 1201 IMAPB codec (validated vs ST 1201 Table 7 + ST 0903 examples). M5 =
standalone VMTI (own UL key + checksum + `registry_by_key()` dispatch) — **both
ST 0903 variants (embedded M3 + standalone M5) supported**. M6 = **VTarget
Series** (0903 Item 101): ST 0903 §9.1.3 Series of VTarget Packs
(`[BER-OID targetId][LS items]`), routed into the VTarget registry, IMAPB inside
packs — Series + packet round-trip byte-exact (matches ST 0903 Figure 13
L=30/13/15). **The core KLV data model is now structurally complete** for
0601 + 0903 (scalars, nesting, packs/series, linear + IMAPB). Next: split the
header-only core into a real lib target + wire CI drift check, then the gstreamer
backend (ADR 0008).

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
  12.5°/10.0° and re-encoding byte-exact. **Cross-checked (2026-07-18) against
  ST 1201 Annex A vectors via [`jmisb`](../context/prior-art-jmisb.md) (MIT) —
  4 ranges incl. the non-zero-Zoffset `IMAPB(-9.9,110,3)`, all pass.** **IMAP
  structural special values (ST 1201 §7.2.3) now implemented** — encode signals
  NaN/±∞/below-min/above-max (`0xD0/0xC8/0xE8/0xE0/0xE1`), decode → IEEE or
  clamped-to-range; validated vs jmisb `IMAPB(0,100,3)` vectors. `is_imap_special`
  helper lets reserialization raw-pass specials (they decode lossily to min/max,
  so don't byte-exact round-trip through the codec alone).
- **Milestone 5 — standalone VMTI (Phase 3)** — restored `ul_key` to the
  `Registry` (generator emits a 16-byte key array; ADR 0010) and added
  `registry_by_key()` UL-key → registry dispatch (demux selection).
  `standalone_roundtrip_test` on a hand-authored `vmti_standalone.klv` (VMTI UL
  key `06.0E.2B.34…03.03.06…` + items + checksum): UL-key dispatch resolves to
  VMTI (not 0601), items decode, and `finalize()` re-encodes byte-exact —
  confirming the VMTI standalone checksum is the ST 0601 algorithm (ST 0903
  §10.1.1 / req 0903.6-119). **Both 0903 variants (embedded M3 + standalone M5)
  now covered.** Cross-refs: ATAK delegates KLV to a closed lib (pgscmedia — no
  source to check); ImpleoTV docs are API-level not byte-level — ST 0903 §10 is
  the authority used.
- **Milestone 6 — VTarget Series (Phase 3)** — new `series.hpp` (`parse_vtarget_series`,
  `build_vtarget_pack`, `build_series`) for ST 0903 §9.1.3 Series of VTarget
  Packs; new `VTARGET_0903` registry (`RegistryId::Vtarget0903`), Item 101 as
  `kind = pack` → child. `vtarget_roundtrip_test` on a hand-authored
  `vmti_vtarget.klv` (matching Figure 13: Item 101 L=30 = two packs L=13/L=15)
  parses/decodes both packs and re-encodes Series + packet byte-exact. Two bugs
  fixed: (1) IMAPB with non-zero `Zoffset` (a<0<b, e.g. offsets IMAPB(-19.2,19.2))
  lost 1 LSB on re-encode — added a few-ULP nudge so IMAPB is a stable inverse;
  (2) `serialize_items()` dropped tag 1 assuming "checksum", but tag 1 is data in
  embedded LSs (VTarget targetCentroid) — removed the 0601-centric assumption
  (checksum handling stays in `finalize()`).
- **Library + packaging + CI (plumbing)** — split the header-only core into a
  compiled `libmisbklv.a` (`src/*.cpp`, declaration-only headers; generated
  constexpr tables moved to `include/misbklv/registry/`). CMake install/export:
  `find_package(misbklv)` → `misbklv::misbklv`, verified with an out-of-tree
  consumer. GitHub Actions CI (`.github/workflows/ci.yml`): build + test
  (LFS + ffmpeg for the `.ts` regression), install smoke test, and the ADR 0012
  registry drift check.

## In progress

- **gstreamer backend — scoping** (see [`backend-scope.md`](./backend-scope.md)).
  Environment probed (gst 1.20.3; all elements present; **`gstreamer-mpegts-1.0`
  dev MISSING** — need `libgstreamer-plugins-bad1.0-dev`). **Key finding**: stock
  `tsdemux` exposes `0x06`+KLVA as `meta/x-klv` (Day/Night/falls) but **silently
  drops `0x15` metadata** (Cheyenne, sync) — extraction is two regimes. Opened
  backend forks 11–14 (interface / 0x15 extraction / `klvpmtrewrite` / optional-dep
  build). `gstreamer-mpegts-1.0` dev **now installed**. **B0 extraction spike
  done**: `tsdemux ! meta/x-klv ! appsink` on Day Flight is **byte-identical** to
  the ffmpeg `.klv` the core already round-trips; appsink yields sub-packet
  fragments (backend reassembles → `parse_packet`); PES PTS unreliable (prefer KLV
  Item 2). **Fork 11 interface drafted — ADR
  [`0013`](../context/decisions/0013-media-backend-interface.md) (PROPOSED)**:
  abstract `MediaBackend`, blocking `extract(source, on_packet)` (borrowed
  per-packet bytes + PTS), `open_insert → Inserter.push/finish` (appsrc
  backpressure); `GstBackend` + `MockBackend`. **Accepted.**
- **B1 done** (extraction + interface + mock + optional-dep build): `backend.hpp`
  (interface), `mock_backend.hpp` (`MockBackend`/`MockInserter`), `gst_backend`
  (real gstreamer extraction: `filesrc ! tsdemux ! appsink`, reassemble +
  `packet_frame_length` framing). `misbklv-gst` optional target (ADR
  [`0014`](../context/decisions/0014-backend-optional-dependency.md), fork 14).
  Tests: `mock_backend` (contract) + **`gst_extract`** — GstBackend extracts
  `Day Flight.mpg` into 6 whole packets, **byte-exact** vs the fixture (14 CTest
  cases). CI installs gstreamer.
- **B2 done** (insertion): `GstInserter` = `appsrc(meta/x-klv) ! mpegtsmux !
  file/udp/srt sink`; `push()` (backpressure) + `finish()` (drain). **Big finding
  — `klvpmtrewrite` NOT needed** (fork 13, ADR
  [`0015`](../context/decisions/0015-no-pmt-rewrite.md)): stock `mpegtsmux` (gst
  1.20.3) already emits `stream_type 0x06`+KLVA, so ADR 0008's PMT-rewrite element
  is superseded. `gst_insert` test: KLV → mux → `.ts` → re-extract **byte-exact**
  (15 CTest cases). Extraction + insertion both work with **stock gstreamer, no
  custom element**. Next: **B3** — 0x15 extraction (fork 12) or **B4** — real-time
  (udp/srt) streaming.

## Next

- **Media backend (ADR 0008)** — implement per [`backend-scope.md`](./backend-scope.md)
  phased plan B0–B4 (extraction spike → interface+mock → insertion+`klvpmtrewrite`
  → 0x15 → real-time). Forks 11–14 to decide as they come up.
- **Registry breadth** (incremental): more 0601 extended items (IMAPB, tags 90+),
  the remaining VTarget items, and Array types (0903 §9.1.2) as data needs them.
- **Known gaps**:
  - Need vectors/data: Report-on-Change trimmed packets (samples are all full);
    a real VMTI stream (M3/M5/M6 use hand-authored fixtures — the new `data/*.ts`
    may help); multi-byte BER-OID tags (≥128, e.g. Item 143 MSID).

## Blockers / notes

- Repo directory still `~/workspaces/libklv` (optional `mv` to `libmisbklv`,
  separate from the name decision).
