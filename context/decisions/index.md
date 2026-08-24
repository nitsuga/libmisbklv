# Decisions — ADR register

Architecture Decision Records: the *why* of resolved forks. This table is the
**register of decided forks** — the roadmap fork #, the ADR that resolved it, and
status. `decision_status` vocabulary and lifecycle in
[`../CONVENTIONS.md`](../CONVENTIONS.md) § Decisions. Forks *without* an ADR yet
(genuinely open) live in
[`../../planning/ROADMAP.md`](../../planning/ROADMAP.md); it points here for the
decided ones rather than duplicating them.

`Fork` is the roadmap fork number (ADR numbering is by creation order, so they
diverge — e.g. fork 7 → ADR 0004; one fork can span several ADRs — fork 4 →
0005/0006/0007).

| Fork | ADR | Status |
|------|-----|--------|
| 1  | [0001 — Build system & C++ standard](./0001-build-system-and-cpp-standard.md) | accepted |
| 2  | [0002 — Project license](./0002-license.md) | accepted |
| 3  | [0003 — Project name collision](./0003-project-name.md) | accepted |
| 7  | [0004 — ADR format & numbering](./0004-adr-format.md) | accepted |
| 4  | [0005 — KLV core data model](./0005-klv-core-data-model.md) | accepted |
| 4  | [0006 — Tag registry (compiled-in)](./0006-tag-registry.md) | accepted |
| 4  | [0007 — Error handling & C ABI](./0007-error-and-c-abi.md) | accepted |
| 5  | [0008 — Media backend — gstreamer (v1)](./0008-media-backend-gstreamer.md) | accepted |
| 6  | [0009 — ST 0604 deferred from v1](./0009-st0604-deferred.md) | deferred |
| 8  | [0010 — Registry descriptor schema](./0010-registry-descriptor-schema.md) | accepted |
| 9  | [0011 — Encode / serialization model](./0011-encode-model.md) | accepted |
| 10 | [0012 — Registry codegen (source format & build)](./0012-registry-codegen.md) | accepted |
| 11 | [0013 — MediaBackend interface](./0013-media-backend-interface.md) | accepted |
| 14 | [0014 — Backend as an optional dependency](./0014-backend-optional-dependency.md) | accepted |
| 13 | [0015 — No klvpmtrewrite (stock mpegtsmux emits 0x06+KLVA)](./0015-no-pmt-rewrite.md) | accepted |
| 12 | [0016 — 0x15 KLV extraction via a gst-free TS demuxer](./0016-ts-0x15-extraction.md) | accepted |
| 15 | [0017 — Real-time streaming (live pacing + idle-timeout)](./0017-realtime-streaming.md) | accepted |
| 16 | [0018 — High-level API (Message + KlvStream/KlvSink facade)](./0018-high-level-api.md) | accepted |
| 17 | [0019 — Cooperative extraction cancellation (stop token)](./0019-extract-cancellation.md) | accepted |
| 18 | [0020 — Video passthrough on the insert path](./0020-video-passthrough.md) | accepted |
| 19 | [0021 — Timestamps on the read path (`KlvPacket::pts_ns`)](./0021-read-path-timestamps.md) | accepted |
| 20 | [0022 — "No output file on failure" spans the insert session](./0022-no-output-on-failure.md) | accepted |
| 21 | [0023 — ST 0604 SEI generation for video passthrough](./0023-st0604-sei-passthrough.md) | accepted |
| 22 | [0024 — ST 0604 SEI generation is opt-in (`Sei0604`)](./0024-sei-generation-opt-in.md) | accepted |
| 23 | [0025 — Build the passthrough chain explicitly instead of with parsebin](./0025-explicit-demuxer-passthrough.md) | accepted |
| 24 | [0026 — Bounded live KLV frame reassembly](./0026-bounded-live-klv-reassembly.md) | accepted |
| 25 | [0027 — High-level streaming errors](./0027-high-level-streaming-errors.md) | accepted |
| —  | [0028 — Hermetic synthetic fixtures replace third-party media](./0028-hermetic-synthetic-fixtures.md) | accepted |
| 26 | [0029 — ST 0903 variable-length uint default encode width](./0029-st0903-variable-uint-default-width.md) | accepted |
| 27 | [0030 — Non-minimal BER long-form length accepted on read](./0030-non-minimal-ber-length.md) | accepted |
| 28 | [0031 — Vendor-neutral live streaming surface — file-or-live video source and multicast sink](./0031-live-streaming-surface.md) | accepted |
| 29 | [0032 — Cooperative insert-drain cancellation (stop token on finish/close)](./0032-cancellable-insert-drain.md) | accepted |
| 30 | [0033 — Generate SEI matching absorbs the encoder DTS-headroom timeline shift](./0033-generate-sei-encoder-timeline-shift.md) | accepted |
| 31 | [0034 — Live video request pads remain linked through EOS drain](./0034-live-request-pad-teardown.md) | accepted |
