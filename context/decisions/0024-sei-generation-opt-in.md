---
type: Decision
title: ST 0604 SEI generation is opt-in (Sei0604::Preserve | Generate)
status: accepted
tags: [decision, 0604, sei, video-passthrough, api, phase-3]
timestamp: 2026-07-28T04:10:00Z
fork: 22
---

# Context

Fork 22. [`0023`](./0023-st0604-sei-passthrough.md) made ST 0604 SEI generation
unconditional: any caller passing a `video_source` got their H.264 access units
rewritten — Picture Timing SEI stripped, a generated Precision Time Stamp
injected — whether they wanted it or not. That was reasonable when exactly one
consumer existed and needed it. It does not generalise:

- It contradicts what video passthrough promised. [`0020`](./0020-video-passthrough.md)
  said the video elementary stream is re-muxed *unchanged*; after 0023 it never
  was, and the test's byte-exact assertion had to be weakened to a size
  comparison to accommodate that.
- Writing the round-trip test for 0023 turned up a case its Context had assumed
  away: **`data/klv_metadata_test_sync.ts` already carries 418 of its own
  `MISPmicrosectime` SEIs.** The premise "sources don't have 0604" holds for
  Parrot MP4s, not generally. On such a source the output carried *two*
  Precision Time Stamps per access unit we timed — the source's and ours, with
  different values and nothing to say which is authoritative.
- The cost is not free: 35 bytes per frame, plus a per-access-unit parse and
  buffer rebuild, imposed on callers who only wanted their video carried across.

So: a consumer that needs ST 0604 in the video ES should ask for it, and a
consumer that does not should get their video back as they handed it over.

# Decision

**Make it a mode on the insert config, defaulting to leaving the stream alone.**

```cpp
enum class Sei0604 { Preserve, Generate };
```

- **`Preserve` (default)** — the video elementary stream is not touched. Source
  ST 0604 SEI passes through intact; a source without any stays without any. No
  probe is attached and no H.264 parser is created, so the cost is zero, and
  `h264parse config-interval` is left at its default rather than forced to `-1`.
  This restores the ADR 0020 property: the output ES is **byte-identical** to
  the source's.
- **`Generate`** — write a Precision Time Stamp into every access unit we can
  time, from ST 0601 item 2 as pushed. The KLV becomes the stream's single
  timestamp authority: any ST 0604 the source carried is **replaced**, and
  Picture Timing SEI is stripped (which is what quiets the SPS-association
  warning that motivated it in the first place).

Reachable three ways, all defaulting to `Preserve`:
`InsertConfig::sei_0604`; a fourth `KlvSink` constructor argument; and a new
`KlvSink(InsertConfig)` overload, since four positional arguments is past the
point where they read.

**Under `Generate`, an unmatched frame is still scanned.** A frame with no KLV
timestamp within tolerance gets no SEI of ours — but the source's is removed
anyway. Half-replacing would leave provenance varying frame to frame with
nothing in the stream to signal which frames mean what; a reader would have no
way to tell a source timestamp from ours. Absence is detectable, silent
inconsistency is not — the same reasoning that removed the relative-PTS fallback
in [`0023`](./0023-st0604-sei-passthrough.md).

# Alternatives considered

- **Keep it always-on** — rejected; that is the complaint. Every consumer pays
  the rewrite for one consumer's requirement, and passthrough stops meaning
  passthrough.
- **Default to `Generate`, opt out** — rejected; it keeps surprising behaviour
  as the default and only helps the one consumer that is already going to have
  to change its pin. "We do not edit a caller's video unless asked" is the
  defensible default.
- **A `bool generate_sei`** — rejected; an enum leaves room for a third mode
  (e.g. generate only where the source has none) without another API break, and
  reads better at the call site than a bare `true`.
- **Generate, but keep the source's SEI too** — rejected; that is the ambiguity
  this fork exists to remove.
- **Generate, but only into access units with no source SEI** — rejected; the
  caller asking us to generate does not reliably get our timestamps, and the
  output mixes provenance invisibly.
- **A separate function rather than a mode** — rejected; the decision is one
  property of an insert session, not a different operation. It composes with
  `realtime` and `video_source` on the same config.

# Consequences

- **Behaviour changes for anyone on `0023`.** Passing a `video_source` no longer
  generates SEI; `parrot-to-klv` opts in explicitly. It was re-pinning anyway.
- **Passthrough is byte-exact again** under the default, and the test asserts it
  rather than the size comparison 0023 had to settle for.
- **One Precision Time Stamp per access unit** under `Generate`, always ours.
- **Frames we cannot time carry no ST 0604 under `Generate`** — including on
  sources that had one. This is a deliberate loss: see the Decision. Callers
  whose KLV covers the whole video (the expected case) never see it.
- **`Preserve` costs nothing** — no parser, no probe, no per-buffer work.
- Source compatibility is kept: every new parameter is defaulted, and the
  existing `KlvSink` constructors still compile unchanged.

# Assumptions / open questions

- **A NAL mixing replaced and unrelated SEI messages is left alone.** Stripping
  is whole-NAL, so it only happens when *every* message in that NAL is one we
  replace — a Picture Timing sharing a NAL with a buffering period survives, and
  with it a source ST 0604 in that same NAL would too. Not observed in the
  samples (`Generate` leaves 0 source SEI on `klv_metadata_test_sync.ts`), and
  the alternative drops bystander messages. Revisit if a real stream packs them.
- **H.265 is unaffected** — `Generate` is accepted and does nothing on non-H.264
  video, which still passes through. Whether that should instead be an error is
  open; it is currently silent.
- Bits 6/5 of the Time Status remain asserted rather than derived
  ([`0023`](./0023-st0604-sei-passthrough.md)).

# Citations

[1] [`0020`](./0020-video-passthrough.md) — the passthrough contract this
    restores under the default.
[2] [`0023`](./0023-st0604-sei-passthrough.md) — the generation this makes
    opt-in; still the source for the payload and matching design.
[3] [`st0604`](../st0604.md) §7 — the SEI being generated or preserved.
[4] [`st0603`](../st0603.md) §7.4 — the Time Status byte carried in it.
