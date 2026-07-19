# libmisbklv — Roadmap

The plan: scope, sequencing, and open forks. Living doc — rewrite freely.
For "where are we right now" see [PROGRESS.md](./PROGRESS.md). Decisions
(resolved rationale) live in [`../context/`](../context/) as `type: Decision`
concepts; this file tracks them as a checklist.

## Scope (v1)

libmisbklv: a C++ library to read and write MISB KLV data — ST 0601 (UAS
Datalink LS) + ST 0903 (VMTI) — from/to MPEG-TS containers via a gstreamer
backend, file or stream (real-time insertion via `appsrc`). See
[ADR 0008](../context/decisions/0008-media-backend-gstreamer.md).

Deferred from v1 (revisit): ST 0604 (ES timestamps —
[ADR 0009](../context/decisions/0009-st0604-deferred.md)), an ffmpeg backend,
ST 0102 security, ST 0807 registry as a data product, ST 1607 amend/segment
beyond MSID passthrough.

## Phases

- **Phase 0 — Foundation** (done): repo + git/LFS, docs structure, OKF
  bundle, standards + prior-art + sample ingests.
- **Phase 1 — Foundational decisions** (done): build system, C++ standard,
  license, name, ADR format (ADRs 0001–0004).
- **Phase 2 — Core architecture + media-backend decisions** (done): data
  model, registry, error + C ABI, gstreamer backend (ADRs 0005–0008).
- **Phase 3 — Implementation** (next): KLV core, registry, `Result<T>`,
  gstreamer backend incl. the in-library PMT-rewrite element. Round-trip
  targets: `../data/Day Flight.mpg`, `../data/Night Flight IR.mpg`.
- **Phase 4 — 0604 ES timestamps** (deferred — [ADR 0009](../context/decisions/0009-st0604-deferred.md)):
  SEI / `user_data` injection/extraction, emulation-prevention stuff/de-stuff.
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
| 5 | Media backend — gstreamer (v1); ffmpeg deferred | DECIDED | [0008](../context/decisions/0008-media-backend-gstreamer.md) |
| 6 | 0604 ES-timestamp layer — deferred from v1 | DEFERRED | [0009](../context/decisions/0009-st0604-deferred.md) |
| 7 | Decision records — ADR format & numbering | DECIDED | [0004](../context/decisions/0004-adr-format.md) |
| 8 | Registry descriptor schema — the field set ADR 0006 punted (mapping-kind + params, special values, length, nested-registry routing) | DECIDED | [0010](../context/decisions/0010-registry-descriptor-schema.md) |
| 9 | Encode / serialization model — owned builder, bottom-up length assembly, forward mapping, checksum/ordering emission | DECIDED | [0011](../context/decisions/0011-encode-model.md) |
| 10 | Registry codegen — descriptor source format (TOML) + build integration (checked-in generated + drift check) | DECIDED | [0012](../context/decisions/0012-registry-codegen.md) |
| 11 | `MediaBackend` interface — pull/push, callback/iterator, buffer ownership, timestamps (backend fork F-A) | PROPOSED | [0013](../context/decisions/0013-media-backend-interface.md) |
| 12 | 0x15 metadata extraction — stock `tsdemux` drops it; custom PID demux vs defer (F-B) | OPEN | — (see [backend-scope](./backend-scope.md)) |
| 13 | `klvpmtrewrite` element form — GstBaseTransform vs pad-probe over `GstMpegtsSection` (F-C) | OPEN | — (see [backend-scope](./backend-scope.md)) |
| 14 | Backend optional-dependency build — `option()` + separate `misbklv-gst` target (F-D) | OPEN | — (see [backend-scope](./backend-scope.md)) |

Forks 8–9 surfaced during the Phase 3 extraction spike (2026-07-17): the core
*decoder* design (forks 4/5) was complete, but the registry descriptor
**schema** and the **encode** path were under-specified. Both are now decided
(ADRs 0010/0011) and will be **locked by a byte-exact round-trip on the spike
packet** as the first implementation milestone — see [PROGRESS.md](./PROGRESS.md).
Fork 10 (the [`0006`](../context/decisions/0006-tag-registry.md) codegen
follow-on — descriptor source format + build integration) is decided in ADR
0012. **All forks resolved; Phase 3 implementation is underway** (milestone 1:
byte-exact round-trip of the spike packet).

When a fork is deliberated, write a `type: Decision` concept in `../context/`
(`status: proposed` → `accepted`), link it here, and update PROGRESS.
