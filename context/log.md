# Knowledge Bundle Log

## 2026-07-19

* **Scope (gstreamer backend)**: probed the environment + extraction path to
  ground ADR [`0008`](./decisions/0008-media-backend-gstreamer.md); wrote
  [`./backend-scope.md`](./backend-scope.md). gst 1.20.3, all
  elements present, **`gstreamer-mpegts-1.0` dev missing** (need
  `libgstreamer-plugins-bad1.0-dev` for the PMT rewrite). **Key finding** (via
  python-gi `tsdemux` pad probe across all samples): stock `tsdemux` exposes
  `0x06`+KLVA as `meta/x-klv` (Day/Night/`falls`) but **silently drops `0x15`
  metadata** (`Cheyenne`, `klv_metadata_test_sync`) — extraction is two regimes,
  and ADR 0008's "accept both on extract" is a real requirement. Opened ROADMAP
  forks 11–14 (interface / 0x15 extraction / `klvpmtrewrite` / optional-dep build).
  Resolved the `0x15` signaling caveat in [`data-samples`](./data-samples.md).
* **B0 extraction spike (backend)**: installed `gstreamer-mpegts-1.0` dev; ran
  `tsdemux ! meta/x-klv ! appsink` on Day Flight via python-gi. Extraction is
  **byte-identical** to the ffmpeg-extracted `.klv` the core already round-trips —
  the core↔gst path is proven. Two interface inputs (recorded in
  [`backend-scope`](./backend-scope.md)): appsink yields sub-packet
  fragments (203 buffers / 6 packets → backend reassembles + `parse_packet`), and
  PES PTS is unreliable (`CLOCK_TIME_NONE`) so correlation should use KLV Item 2.
* **Decision (proposed)**: Fork 11 →
  [`0013-media-backend-interface`](./decisions/0013-media-backend-interface.md)
  (proposed). Abstract `MediaBackend`: blocking `extract(source, on_packet)`
  yielding framed per-packet `KlvPacket{bytes, pts_ns}` (bytes borrow the
  backend's reassembly buffer — the ADR 0011 boundary; consumer runs
  `parse_packet`) + `open_insert(config) → Inserter{push, finish}` with appsrc
  backpressure. Byte-level (no decode); `Result<T>`; `GstBackend` + `MockBackend`;
  interface header core-only (gstreamer confined to `GstBackend`, fork 14).
  Rejected: pull-iterator, parsed-`Packet` units, raw-buffer units,
  need-data-callback insertion, no-interface. Grounded by the B0 spike. ROADMAP
  fork 11 → PROPOSED.
* **Decision (accepted)**: [`0013`](./decisions/0013-media-backend-interface.md)
  → accepted. `MediaBackend` interface locked; B1 (GstBackend extraction +
  MockBackend + optional-dep CMake) implements it. ROADMAP fork 11 → DECIDED.
* **Decision (accepted)**: Fork 14 →
  [`0014-backend-optional-dependency`](./decisions/0014-backend-optional-dependency.md)
  (accepted): a **separate `misbklv-gst` CMake target** holds the gstreamer impl;
  the core `misbklv` stays dependency-free. `option(MISBKLV_GSTREAMER)` +
  `pkg_check_modules`; skipped if gstreamer absent. Interface/factory headers are
  gstreamer-free. ROADMAP fork 14 → DECIDED.
* **B1 (implementation)**: media backend — extraction + interface + mock.
  `include/misbklv/backend.hpp` (ADR 0013 interface: `MediaBackend`, `KlvPacket`,
  `Inserter`), `mock_backend.hpp` (`MockBackend`/`MockInserter`, core-only),
  `gst_backend.hpp` factory + `src/gst/gst_backend.cpp` (real gstreamer:
  `filesrc ! tsdemux ! appsink`, reassemble appsink fragments, frame packets via
  new `packet_frame_length`). Added `Error::{Backend,Unsupported}`. Tests:
  `mock_backend` (contract) + **`gst_extract`** — GstBackend extracts
  `Day Flight.mpg` into **6 whole packets, byte-exact vs the committed `.klv`**
  (each `parse_packet`-able). 14 CTest cases; CI installs gstreamer. Insertion is
  B2 (`open_insert` returns `Unsupported`).
* **Decision (accepted)**: Fork 13 →
  [`0015-no-pmt-rewrite`](./decisions/0015-no-pmt-rewrite.md) (accepted).
  **`klvpmtrewrite` is NOT needed.** B2 spike (gst 1.20.3): stock
  `appsrc(meta/x-klv) ! mpegtsmux` already emits `stream_type 0x06` + `KLVA`
  registration descriptor; insert→re-extract is byte-exact. Supersedes ADR 0008's
  PMT-rewrite requirement (its premise was true only for older gstreamer). ADR
  0008 annotated; ROADMAP fork 13 → DECIDED.
* **B2 (implementation)**: media backend insertion. `GstInserter` in
  `gst_backend.cpp` = `appsrc(meta/x-klv) ! mpegtsmux ! {file,udp,srt} sink`;
  `push()` (blocks on backpressure) + `finish()` (EOS/drain); `make_sink()` parses
  `file:`/`udp:`/`srt:`. `open_insert` builds+starts the pipeline. `gst_insert`
  test: KLV → mux → `.ts` → re-extract **byte-exact** (15 CTest cases). Both
  directions now work with **stock gstreamer, no custom element** — B2 collapsed
  from "build klvpmtrewrite" to a thin inserter.
* **Decision (accepted)**: Fork 12 →
  [`0016-ts-0x15-extraction`](./decisions/0016-ts-0x15-extraction.md) (accepted):
  a gst-free MPEG-TS KLV extractor for the `0x15` streams stock `tsdemux` drops.
* **B3 (implementation)**: `0x15` extraction. PES probe showed `0x15` wraps KLV in
  SMPTE RP 217 metadata AU cells (`stream_id 0xfc`, 5-byte cell header) vs `0x06`
  (KLV directly in PES). New core `extract_ts_klv` (`ts.hpp`/`src/ts.cpp`, in the
  `misbklv` lib, no gstreamer): iterates 188-byte TS packets, finds the KLV PID by
  content (PES payload starting with a SMPTE UL), reassembles PES, unwraps the AU
  cell for `0x15`, frames via `packet_frame_length`. `ts_extract_test`: **byte-exact
  vs ffmpeg** on Day Flight (0x06), Cheyenne + sync (0x15); each unit
  `parse_packet`-able. 18 CTest cases. **Bonus: file KLV extraction is now
  gstreamer-free** — gstreamer only needed for live sources + mux.
* **Decision (accepted)**: Fork 15 →
  [`0017-realtime-streaming`](./decisions/0017-realtime-streaming.md) (accepted):
  live-sink clock pacing + live-source idle-timeout termination. Rejected a
  handler stop-token (changes the ADR 0013 signature), over-the-wire EOS (udp has
  none), always-on `sync`, and RTP payloading (deferred).
* **B4 (implementation)**: real-time streaming, closing the ADR 0008 goal.
  `open_insert` honors `InsertConfig::realtime` (`appsrc is-live` + sink `sync` →
  clock-paced, `push` blocks at stream rate); `extract` gained `make_src` so a
  `udp:`/`srt:` source uses `udpsrc`/`srtsrc` and ends on a `udpsrc` idle timeout
  (500 ms, ignored until ≥1 packet flows so startup can't truncate) since no EOS
  crosses the wire. `gst_stream_test`: one-process **udp loopback** (receiver
  `extract()` on a thread, realtime `push` on main) → **byte-exact**, 6 packets in
  ~165 ms (5×33 ms = genuinely clock-paced), stable across repeated runs.
  19 CTest cases. **Backend B0–B4 complete**; extraction + insertion, file + live,
  all on stock gstreamer with no custom element.
* **Restructure (planning/context seams)**: de-duplicated the ripple-update
  workflow. [`decisions/index.md`](./decisions/index.md) gained a Fork column and
  is now the sole **decided-fork register**; [`ROADMAP`](../planning/ROADMAP.md)
  dropped its duplicate fork table (keeps scope / phases / *open* forks, defers to
  the register). [`PROGRESS`](../planning/PROGRESS.md) is now present-tense only —
  its ~200-line "Done" history graduated here (this log owns chronology).
  [`backend-scope.md`](./backend-scope.md) moved `planning/` → `context/` as a
  `type: Component` concept (durable design analysis, not plan/state). Root
  `CLAUDE.md` + [`CONVENTIONS`](./CONVENTIONS.md) updated to codify the
  one-job-per-doc split.
* **Conventions (doc hygiene)**: two ripple-workflow rules, no tooling. (1)
  Decision log entries are a **single thin line** (chronology + link; the ADR owns
  the rationale — [`CONVENTIONS`](./CONVENTIONS.md) § Decisions), a second entry
  only on a cross-session gap or revision. (2) **No live tallies** (test-case
  counts, item totals) in present-tense / durable prose — a specific number
  belongs only in a dated log snapshot here (root `CLAUDE.md` Planning hygiene).
* **Hardening (implementation)**: real-world robustness pass — inputs the clean
  vendor samples never exercised. Fixed **integer-overflow → OOB** in the length
  arithmetic (`parse_packet`, `parse_items`, `packet_frame_length`: a crafted
  8-byte BER length wrapped past a naive bound check into an out-of-range
  `subspan`), and added **length validation to `codec::decode`** (numeric kinds
  reject 0- / >8-byte values — a 0-length item drove shift-by-negative UB; also
  made the signed-linear `imax` shift unsigned to avoid `1ll<<63`). Confirmed
  **multi-byte BER-OID tags (≥128)** already parse/build correctly (test gap, not
  a bug — `read_oid`/`write_oid` handle base-128) and **Report-on-Change trimmed**
  packets round-trip. New `hardening_test` covers all three; the core builds clean
  under **ASan+UBSan** (`MISBKLV_SANITIZE` option + a CI sanitizer job, gstreamer
  off). Verified the test has teeth: reintroducing the overflow flips it to FAIL.
* **Decision + API (fork 16)**: high-level facade —
  [`0018`](./decisions/0018-high-level-api.md) (accepted). `Message` (core): an
  owned, editable packet — copy+parse, registry auto-selected by UL key, typed
  `get<T>`/`set`, `encode()` byte-exact for untouched items (move-only; owns above
  the borrow core). `KlvStream`/`KlvSink` (`misbklv-gst`): range-for read (backend
  push-extract adapted to pull via a background thread + bounded queue), edit,
  emit — file + live. `message_test` + end-to-end `api_test` (read Day Flight,
  edit SensorLatitude, emit, re-read — all 6 frames' edits persist through
  decode→encode→mux→demux), a `klv_edit` example, and the first user guide
  [`docs/api.md`](../docs/api.md). Closes the Phase-3 usability pass.

## 2026-07-18

* **Ingest (Prior Art)**: Added [`jmisb`](./prior-art-jmisb.md) (WestRidgeSystems,
  Java, **MIT**) — the most complete peer found (0601/0903/1201/0102 + ~20 more).
  Generic KLV core (Ber*/KlvParser/LdsParser/UdsParser/UniversalLabel/ArrayBuilder)
  + **class per item** (the [`0006`](./decisions/0006-tag-registry.md)-rejected
  approach — a foil, not a template). Value: **JUnit byte-array test vectors** as
  ground truth for our hand-authored fixtures, and `st1201/FpEncoder` +
  `OutOfRangeBehaviour` to cross-check our IMAP (Zoffset edge + unimplemented IMAP
  special-value semantics). Flags concepts we deferred: UDS (vs LDS) and Array type
  (0903 §9.1.2). MIT → reference freely, attribute if lifting a vector verbatim.
  Index Prior Art section updated. Cross-check of vectors: next.
* **Cross-check (IMAP)**: ran our `imapb` codec against ST 1201 Annex A vectors
  transcribed by [`jmisb`](./prior-art-jmisb.md) (`FpEncoderThreeByteTest`,
  `ST1201AnnexATest`). Added 8 vectors to `imapb_test` across 4 ranges —
  `IMAPB(0,100,3)`, **`IMAPB(-9.9,110,3)` (non-zero Zoffset)**, `IMAPB(0.1,0.9,2)`
  — **all pass** (e.g. `IMAPB(-9.9,110,3)` enc(0.0)=0x09E667, enc(110)=0x77E667).
  Independent confirmation that our IMAP forward mapping + the M6 Zoffset fix are
  correct. **Gap found**: the vectors include IMAP *structural* special values
  (NaN=0xD0.., ±∞=0xC8/0xE8.., below/above = 0xE0/0xE1.., top-2-MSB signaling)
  which our codec does not yet handle (only explicit per-item patterns) — logged
  to PROGRESS Known gaps; jmisb's `OutOfRangeBehaviour` is the reference.
* **Cross-check (0601/0903)**: new `jmisb_crosscheck_test` (8th CTest case)
  validates our registered codecs against [`jmisb`](./prior-art-jmisb.md) item
  test vectors (MIT). **0601 linear-LDS** (not covered by the IMAPB-only ST 1201
  vectors): SensorLatitude (int32 ±90: −90=0x80000001, 90=0x7FFFFFFF,
  60.1768229669783=0x5595B66D), SensorTrueAltitude (uint16 −900..19000:
  14190.72=0xC221), PlatformHeadingAngle (uint16 0..360: 159.9744=0x71C2) — all
  byte-exact, **independently confirming 0601 lat/lon/angles are legacy linear,
  not IMAP** (the M-era KB correction to [`st0601`](./st0601.md)). **0903 VTarget**:
  targetLocationOffsetLat = IMAPB(−19.2,19.2,3) (10.0°=0x3A6667, confirms our
  registry range matches jmisb) and targetColor uint24 (RGB 85,136,51=0x558833).
  jmisb's 0601/0903 messages are built dynamically (no single golden packet), so
  this cross-checks per-item codecs; full-packet vectors remain future work.
* **Research (0604 / post-v1)**: scanned [`jmisb`](./prior-art-jmisb.md) for ST
  0604 — **none** (no SEI/NAL/H.26x in the whole repo; KLV-only). Confirms 0604
  is a video-ES subsystem, not KLV. Reusable seam = **ST 0603 time** (jmisb
  `st0603`, = our 0601 item-2 semantics); only the ES-embedding is new. Recorded
  on [`0009`](./decisions/0009-st0604-deferred.md) (revisit notes) + the jmisb
  prior-art doc.
* **Characterize (Sample Data)**: extracted + walked the three new `data/*.ts`
  ([`data-samples`](./data-samples.md)). **All ST 0601, no VMTI** (tag 74 absent
  everywhere — 0903 read path stays on hand-authored + jmisb vectors). Cheyenne
  (407 pkts), falls (**two** KLV streams: basic ≤tag65 + extended ≤tag91, 1953
  each), sync (365). **All four streams round-trip byte-exact = 4678 real packets**
  through parse→codec→builder→checksum — strongest validation to date. Surfaced:
  registry-breadth items (3/4/10 strings, 26–33 corner offsets, falls-2's 90+
  IMAPB extended items); **tag 48 = Security LS (ST 0102)** nested instance;
  multi-byte BER-OID tags (≥128) still unexercised (max tag 91). Not yet wired as
  CTest (needs committed `.klv` fixtures or build-time ffmpeg extraction).
* **Regression tests (build-time extraction)**: wired the `data/*.ts` as CTest
  cases (`stream_cheyenne`/`falls`/`falls_ext`/`sync`). CMake `find_program(ffmpeg)`
  + `add_custom_command` extracts each KLV ES to `build/*.klv` at build time (ALL
  target `extract_ts_klv`); no committed `.klv` bloat. Skipped if ffmpeg absent;
  CI must `git lfs pull` the `.ts` first. 12 CTest cases total, all pass.
* **IMAP special values (implementation)**: closed the gap the jmisb cross-check
  found. `codec.hpp` now handles ST 1201 §7.2.3 structural specials (top-2-bits
  set): `imapb_special_of` classifies by the Table 2/3 patterns; `imapb_encode`
  signals NaN→`0xD0`, +∞→`0xC8`, −∞→`0xE8`, x<min→`0xE0` (BELOW), x>max→`0xE1`
  (ABOVE); `imapb_decode` returns IEEE values or clamps below/above→min/max (ST
  1201 default reverse). Public `is_imap_special()` for reserialization
  raw-passthrough (below/above decode lossily, so don't round-trip through the
  codec alone). `imapb_test` gained the jmisb `IMAPB(0,100,3)` special vectors —
  all pass. No regressions (12/12).
* **Library split (plumbing)**: converted the header-only core into a compiled
  static library `libmisbklv.a`. Moved function/method bodies from `ber`/`codec`/
  `packet`/`builder`/`series`/`registries` headers into `src/*.cpp`; public
  headers are now declaration-only. `types.hpp` (types + `Result<T>` template +
  `Registry::find`) and the generated `constexpr` tables stay header (must, for
  templates/compile-time). Internal codec helpers moved to an anonymous namespace
  in `codec.cpp`; public codec API narrowed to what's used cross-TU
  (`rd_uint`/`decode`/`encode`/`imapb_*`/`bcc16`/`is_imap_special`). Added
  umbrella `misbklv.hpp`. CMake: `INTERFACE` → `STATIC` target. All 12 tests link
  the lib and pass. Remaining plumbing: install/export + CI config.
* **Install/export + CI (plumbing)**: made the library installable and consumable.
  Moved the generated tables `src/registry/` → `include/misbklv/registry/` (clean
  single-`include/` install tree; dropped `src` from public include dirs, updated
  includes + the regenerate target, which also gains the previously-missing
  vtarget0903). CMake `install(TARGETS EXPORT)` + `configure_package_config_file`
  → `find_package(misbklv)` gives `misbklv::misbklv`; **verified end-to-end** by
  installing to a prefix and building an out-of-tree consumer. (Bug caught + fixed:
  `include(GNUInstallDirs)` must precede the `INSTALL_INTERFACE` include-dir GE, or
  it's empty.) Added `.github/workflows/ci.yml`: build+test (LFS + ffmpeg), install
  smoke test, and the ADR 0012 drift check job. ADR 0012 path refs updated.

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
  and the embedded fallback reader (drift check).
* **Milestone 2 (implementation)**: full-stream byte-exact round-trip.
  `stream_roundtrip_test` walks every packet in both samples and reconstructs the
  whole stream identically (Day Flight 6/6, Night Flight IR 18/18). Registry
  expanded to the full 25-tag sample set (26 descriptors; ranges from 0601.19
  §8). Two findings, both faithful to the ADRs: (1) **length-preserving
  reserialization** — wire length ≠ canonical length (`Day Flight` encodes Item
  22 Target Width as 4 B vs 0601.19 uint16), so `codec::encode` / `set()` are now
  length-parameterized (ADR 0011 updated); (2) **mandatory-enforcement opt-out**
  on `finalize()` for Report-on-Change packets (these samples are all full
  packets, so RoC trimming isn't yet exercised). Fixtures `dayflight.klv` /
  `nightflight_ir.klv` added.
* **Milestone 3 (implementation)**: 0903 nesting round-trip. Restored
  `RegistryId` + descriptor `child` (ADR 0010) + a `registry_for()` resolver;
  added the VMTI registry (`registry/vmti0903.toml`), recursive `parse_items`
  (bare TLV walk) and `LocalSetBuilder::serialize_items()` (nested value, no
  key/checksum). `nested_roundtrip_test` on a hand-authored fixture
  (`vmti_nested.klv` via `make_vmti_fixture.py`) — a 0601 packet nesting a VMTI
  LS in Item 74 — passes: childRegistry routing OK, nested + outer both
  byte-exact. Generator extended (`child`/`nested_ls`; variable-length uint from
  omitted length). Fixed a stale 16-vs-17-byte UL-key hex typo in unused TOML
  `ul_key` fields + the fixture script. Scalar-only; VTarget Series → M5.
 
* **Milestone 4 (implementation)**: real ST 1201 IMAPB codec. `ValueKind::IMAPB`
  now uses the Starting-Point-B mapping (`imapb_params`: bPow/dPow/sF/sR/Zoffset;
  `imapb_decode`/`imapb_encode`), split from the linear-LDS path in `codec.hpp`.
  `imapb_test` validates against the standards' own worked examples — ST 1201 §10
  Table 7 (IMAPB(-900,19000), L=3: enc(10.0)=0x038E00, plus 0.0/-900 vectors) and
  ST 0903 §10.1.11 (IMAPB(0,180), L=2: 12.5°=0x0640) — plus a full-range
  round-trip sweep. VMTI FoV items 11/12 (IMAPB) added to the registry + nested
  fixture for end-to-end coverage; 5th CTest case. VTarget Series → M5.
 
* **Milestone 5 (implementation)**: standalone VMTI. Restored `ul_key` to the
  `Registry` struct (generator emits a 16-byte key array + validates length);
  added `registry_by_key()` for UL-key → registry demux dispatch. New
  `standalone_roundtrip_test` on a hand-authored `vmti_standalone.klv` (VMTI UL
  key + items + checksum): dispatch resolves to VMTI (not 0601), decodes, and
  re-encodes byte-exact. Confirmed via ST 0903 §10.1.1 / **req 0903.6-119** that
  the standalone VMTI checksum is the ST 0601 16-bit-BCC algorithm over the whole
  LS (key..checksum-length) — so `finalize()` was already correct. **Both 0903
  variants now supported + tested: embedded (M3, Item 74) and standalone (M5).**
  6th CTest case. Cross-references evaluated per user request: **ATAK**
  (`VideoDropDownReceiver.java`) delegates KLV to the closed `com.partech.pgscmedia`
  lib — no parsing source to check against; **ImpleoTV MisbCore** docs are
  API-level (JSON), not byte-level, and ship no downloadable `vmtiPckt.bin` (that
  name is only a placeholder path in sample code) — ST 0903 §10 is the authority
  used. VTarget Series → M6.
* **Milestone 6 (implementation)**: VTarget Series (0903 Item 101). New
  `series.hpp` (`parse_vtarget_series` / `build_vtarget_pack` / `build_series`)
  for ST 0903 §9.1.3 Series of VTarget Packs (`[BER-OID targetId][LS items]`);
  new `VTARGET_0903` registry + `RegistryId::Vtarget0903`; Item 101 = `kind=pack`
  → child. `vtarget_roundtrip_test` on `vmti_vtarget.klv` (ST 0903 Figure 13:
  L=30 = two packs L=13/L=15) round-trips Series + packet byte-exact; 7th CTest
  case. **KLV core now structurally complete for 0601+0903** (scalars, nesting,
  packs/series, linear + IMAPB). Two bugs found + fixed against real structure:
  (1) IMAPB non-zero-`Zoffset` re-encode lost 1 LSB — added a few-ULP nudge to
  `imapb_encode` so decode∘encode is stable (imapb_test gained the IMAPB(-19.2,
  19.2) 10.0°=0x3A6667 vector); (2) `serialize_items()` skipped tag 1 as
  "checksum", wrong for embedded LSs where tag 1 is data (VTarget targetCentroid)
  — removed the assumption; checksum handling stays in `finalize()`. (Detail in
  PROGRESS.)
