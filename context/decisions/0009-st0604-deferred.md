---
type: Decision
title: ST 0604 (ES timestamps) — deferred from v1
status: deferred
tags: [decision, 0604, es-timestamps, deferred, phase-2]
timestamp: 2026-07-17T21:00:00Z
fork: 6
---

# Context

Fork 6. ST 0604 (Precision / Nano / Commercial timestamps embedded in the
*video elementary stream* via SEI / `user_data`) is a **separate layer** from
the KLV metadata stream — [`0005`](./0005-klv-core-data-model.md)–[`0008`](./0008-media-backend-gstreamer.md)
handle KLV 0601/0903. It is a project target but a **secondary** one.
Implementing it means a custom gstreamer SEI-injector element + NAL/SEI
manipulation + `0xFF` emulation-prevention stuff/de-stuff — comparable effort
to the PMT-rewrite element ([`0008`](./0008-media-backend-gstreamer.md)), and
the same per-frame-sync shape as the sync-KLV (`0x15`) work deferred in
[`0008`](./0008-media-backend-gstreamer.md). [`0601`](../st0601.md)'s KLV
stream already carries a Precision Time Stamp (item 2), covering
metadata↔video correlation for the common case.

# Decision

**Defer ST 0604 from v1.** v1 = 0601/0903 KLV read/write via gstreamer
([`0008`](./0008-media-backend-gstreamer.md)); the 0604 ES-timestamp layer is
revisited after v1. The core KLV value proposition does not depend on it.

**Partially resolved (2026-07-27) — see [`0023`](./0023-st0604-sei-passthrough.md).**
A consumer need pulled the *generation* half forward ahead of v1: on the video
passthrough path ([`0020`](./0020-video-passthrough.md)) we now write H.264
Precision Time Stamp SEI from the KLV item-2 timestamp. This ADR stays
`deferred` because what it defers is broader than what 0023 delivered. Still
deferred: **reading** 0604 back out of a source ES, H.265 Nano
(ST 0604.6 §8), Commercial time code, H.262 `user_data`, and any 0604 handling
off the video-passthrough path.

# Alternatives considered

- **0604 in v1** — rejected for v1: a second custom gstreamer element
  (SEI-injector) + NAL/SEI surgery, roughly doubling the
  custom-element / NAL-surgery work, in the same per-frame-sync shape already
  deferred (sync-KLV `0x15`). Not justified for a secondary target.
- **Drop 0604 entirely** — rejected: it remains a project target (secondary);
  deferral (not removal) keeps it on the roadmap.

# Consequences

- v1 standards coverage: **0601 / 0903** (KLV); ST 0604 deferred —
  `CLAUDE.md` / `README` reconciled on accept. (Both restated on
  2026-07-27 to scope the deferral to what [`0023`](./0023-st0604-sei-passthrough.md)
  left open.)
- The [`0604`](../st0604.md) concept doc stays in the KB as reference for when
  it's revisited.
- When started, 0604 will need: an in-library SEI-injector gstreamer element
  (like the PMT-rewrite in [`0008`](./0008-media-backend-gstreamer.md)),
  NAL access-unit-boundary handling, `0xFF` emulation-prevention stuff/de-stuff,
  H.264/H.265 (H.262 optional), and timestamp encode/decode.

# Assumptions / open questions (trigger for revisiting)

- Revisit when: frame-accurate ES-embedded timestamps are required independent
  of KLV availability, OR a consumer needs 0604 specifically, OR the sync-KLV
  per-frame work (deferred in [`0008`](./0008-media-backend-gstreamer.md)) is
  picked up — 0604's per-frame SEI injection is the same shape, so doing them
  together is natural.
- Scope when revisited: Precision Time Stamp first; Nano (H.265-only) and
  Commercial time-code as follow-ons.
- **Revisit trigger fired (2026-07-27):** the "a consumer needs 0604
  specifically" condition above is what opened fork 21 — see
  [`0023`](./0023-st0604-sei-passthrough.md) for the generation side. The
  remaining deferred scope is listed under Decision.
- **Prior-art scan (2026-07-18):** [`jmisb`](../prior-art-jmisb.md) (broad KLV
  lib, 20+ standards) implements **no 0604** — no SEI/NAL/H.26x anywhere —
  independently confirming 0604 is a *video-ES* subsystem, not KLV, and won't
  come from a KLV library. The one reusable seam is **ST 0603 time** (jmisb has
  `st0603`; = our [`0601`](../st0601.md) item-2 timestamp semantics): 0604
  payloads are 0603-based, so timestamp *value* handling is shared and only the
  ES-embedding is new (SEI / `user_data`, the 16-byte identifiers, `0xFF`
  stuffing). References for that half: gstreamer/ffmpeg bitstream tooling
  (h264/h265parse SEI) + [`gstklvplugin`](../prior-art-gstklvplugin.md).

# Citations

[1] [`st0604`](../st0604.md) — the ES-timestamp standard (reference preserved).
[2] [`0008`](./0008-media-backend-gstreamer.md) — sync-KLV per-frame deferral
    (same shape); the PMT-rewrite element precedent.
[3] [`st0601`](../st0601.md) — item 2 Precision Time Stamp (covers the common
    correlation need without 0604).
