# libmisbklv — Roadmap

The plan: scope, sequencing, and **open** forks. Living doc — rewrite freely.
For "where are we right now" see [PROGRESS.md](./PROGRESS.md). Resolved forks and
their rationale live in [`../context/`](../context/) as `type: Decision` concepts;
the **decided register** (fork # ↔ ADR ↔ status) is
[`../context/decisions/index.md`](../context/decisions/index.md). This file no
longer duplicates it — it tracks scope, phases, and forks still open.

## Scope (v1)

libmisbklv: a C++ library to read and write MISB KLV data — ST 0601 (UAS
Datalink LS) + ST 0903 (VMTI) — from/to MPEG-TS containers via a gstreamer
backend, file or stream (real-time insertion via `appsrc`). See
[ADR 0008](../context/decisions/0008-media-backend-gstreamer.md).

Deferred from v1 (revisit): most of ST 0604 (ES timestamps —
[ADR 0009](../context/decisions/0009-st0604-deferred.md); its H.264 *generation*
side landed early on the video passthrough path, opt-in —
[ADR 0023](../context/decisions/0023-st0604-sei-passthrough.md),
[ADR 0024](../context/decisions/0024-sei-generation-opt-in.md)), an ffmpeg backend,
ST 0102 security, ST 0807 registry as a data product, ST 1607 amend/segment
beyond MSID passthrough.

## Phases

- **Phase 0 — Foundation** (done): repo + git/LFS, docs structure, OKF
  bundle, standards + prior-art + sample ingests.
- **Phase 1 — Foundational decisions** (done): build system, C++ standard,
  license, name, ADR format (ADRs 0001–0004).
- **Phase 2 — Core architecture + media-backend decisions** (done): data
  model, registry, error + C ABI, gstreamer backend (ADRs 0005–0012).
- **Phase 3 — Implementation** (done, extending incrementally): the KLV core
  (milestones 1–6: round-trips, ST 1201 IMAPB, 0903 nesting / standalone /
  VTarget Series) and the gstreamer backend (every B-phase in
  [`backend-scope`](../context/backend-scope.md): 0x06 extraction file + live,
  gst-free 0x06/0x15 offline extraction; insertion file + live, clock-paced,
  with optional video passthrough) —
  both on stock gstreamer, no custom element. The in-library PMT-rewrite element that ADR 0008 planned
  was proven unnecessary ([ADR 0015](../context/decisions/0015-no-pmt-rewrite.md)).
  Ongoing within this phase: registry breadth (below).
- **Phase 4 — 0604 ES timestamps** (partly landed early, remainder deferred —
  [ADR 0009](../context/decisions/0009-st0604-deferred.md)): H.264 Precision
  Time Stamp SEI *generation* (with emulation-prevention stuffing) came forward
  into Phase 3 for a consumer, opt-in
  ([ADR 0023](../context/decisions/0023-st0604-sei-passthrough.md),
  [ADR 0024](../context/decisions/0024-sei-generation-opt-in.md)). Still here:
  reading 0604 back out of an ES, H.265 Nano, Commercial time code, H.262
  `user_data`.
- **Phase 5 — Hardening** (largely landed, continuing alongside Phase 3): the
  adversarial/real-world test pass (`hardening_test`, ASan+UBSan in CI),
  conformance against the standards' own worked examples
  (`st0601_examples_test`) and jmisb, `find_package` packaging with a consumer
  smoke test, and the user guide ([`../docs/api.md`](../docs/api.md)); bounded
  live-frame reassembly and high-level streaming errors are settled in
  [ADR 0026](../context/decisions/0026-bounded-live-klv-reassembly.md) and
  [ADR 0027](../context/decisions/0027-high-level-streaming-errors.md). What
  keeps it open is data-driven: conformance breadth follows the samples and the
  registry (see PROGRESS "Known gaps").

## Open forks

A *fork* is a decision point — a branch in the plan/design that needs a choice
(see [`../context/CONVENTIONS.md`](../context/CONVENTIONS.md) § Decisions).

Status legend: `OPEN` (undiscussed) · `PROPOSED` (Decision concept written,
`decision_status: proposed`) · `DECIDED` / `DEFERRED` (has an ADR — see the
register).

**One open fork remains**; all other design decisions are resolved. See the
decided register ([`../context/decisions/index.md`](../context/decisions/index.md))
for the resolved fork → ADR mapping.

- **Fork 34 / issue #60 — live insert source liveness.** Decide the public
  signal for video-source progress: a last-delivery timestamp is reliable even
  when `mpegtsmux` cannot forward a branch EOS while its KLV pad is open;
  determine whether exposing the branch EOS event adds useful information.

**Candidate future forks** (not yet opened), mostly downstream of
registry/data breadth:
- ST 0102 Security LS as a typed nested registry (seen as tag 48 in `falls`;
  currently opaque passthrough).
- 0903 Array type (§9.1.2) — a descriptor-schema extension.
- RTP payloading for the live path — deferred by
  [ADR 0019](../context/decisions/0019-extract-cancellation.md).
- **Live 0x15 metadata extraction** — real-time reading of `stream_type 0x15`
  streams. Today 0x15 is offline-only (the whole-buffer `extract_ts_klv`); the
  live gst path uses `tsdemux`, which silently drops 0x15. Needs a *streaming*
  incremental TS demux (stateful `feed()` handling partial-packet spans across
  live buffers) fed from a raw `udpsrc`/`srtsrc ! appsink` that **bypasses
  `tsdemux`** — which would also unify 0x06 + 0x15 onto one gst-free live path.
  The fork is that path vs. a `tsdemux` workaround (none found — see
  [ADR 0016](../context/decisions/0016-ts-0x15-extraction.md)); extends 0016.

Incremental *work* that needs no fork (registry breadth, an SRT test) lives in
[PROGRESS](./PROGRESS.md) "Next", not here.

## Fork lifecycle

When a fork is deliberated, write a `type: Decision` concept in `../context/`
(`decision_status: proposed` → `accepted`), add its row to the decided register
([`../context/decisions/index.md`](../context/decisions/index.md)), and update
PROGRESS. A genuinely open (undeliberated) fork gets a line under **Open forks**
above until it has an ADR.
