# libmisbklv — Progress

Current status. Volatile; rewrite each session. For the plan, see
[ROADMAP.md](./ROADMAP.md).

## Now

Phase 0 done. Phase 1 complete. Phase 2 (core architecture + media backend)
decided — forks 4 (0005/0006/0007), 5 (0008) accepted; fork 6 (0604) deferred.
Phase 3 (implementation) **started with an extraction spike** on `Day
Flight.mpg`, which validated the read path (BER walk, timestamp, lat/lon,
checksum) and surfaced two design gaps the "all forks resolved" status missed.
Forks 8 (descriptor schema, ADR 0010) and 9 (encode model, ADR 0011) are both
**accepted** — the design backlog is clear. Next is code: the first milestone is
a byte-exact round-trip of the spike's first packet (decode → re-encode →
identical), which locks 0010 + 0011 against real bytes.

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

## In progress

- **Extraction spike (done)** — `ffmpeg` demux of `Day Flight.mpg` KLV PID
  (`0x1F1`) + a throwaway parser walked the first 0601 packet: BER length, TLV
  walk, Item 2 timestamp, sensor lat/lon, and a **verified 16-bit checksum**.
  Confirmed the legacy-linear vs IMAPB mapping split (see
  [`st0601`](../context/st0601.md) § Encoding + `log.md`). Grounds forks 8/9.

## Next

- **Milestone 1 — byte-exact round-trip** (the acceptance gate for forks 8 + 9):
  decode the spike's first `Day Flight.mpg` packet → re-encode via the builder →
  assert identical bytes (checksum included). Exercises the data model
  ([`0005`](../context/decisions/0005-klv-core-data-model.md)), a minimal
  descriptor table ([`0010`](../context/decisions/0010-registry-descriptor-schema.md)),
  IMAP/linear codecs both directions, and the builder
  ([`0011`](../context/decisions/0011-encode-model.md)) in one test.
- **Then build out** against the locked design — KLV core + registry codegen,
  `Result<T>` ([`0007`](../context/decisions/0007-error-and-c-abi.md)), 0903
  nesting, then the gstreamer backend incl. the in-library PMT-rewrite element
  ([`0008`](../context/decisions/0008-media-backend-gstreamer.md)). Round-trip
  targets: `data/Day Flight.mpg`, `data/Night Flight IR.mpg`.
- **Pending small decision** (0006 follow-on): the descriptor source-of-truth
  file format + codegen tool (lean: TOML/JSON + Python generator) — settle
  before the registry table is authored.

## Blockers / notes

- Repo directory still `~/workspaces/libklv` (optional `mv` to `libmisbklv`,
  separate from the name decision).
