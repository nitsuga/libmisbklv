# libmisbklv — Progress

Current status only — **present tense, volatile, rewrite each session**. The
chronological history (what landed when, milestone/decision detail) lives in
[`../context/log.md`](../context/log.md); the decided-fork register is
[`../context/decisions/index.md`](../context/decisions/index.md); the plan is
[ROADMAP.md](./ROADMAP.md). This file does not re-narrate the past.

## Now

Phases 0–2 (foundation + all design forks) done. **Phase 3 implementation is
done and extending incrementally** — the state today:

- **KLV core — structurally complete for 0601 + 0903** (scalars, nesting,
  packs/series, linear-LDS + ST 1201 IMAPB incl. special values). Milestones 1–6
  landed (detail in [`log.md`](../context/log.md)).
- **gstreamer media backend — complete (B0–B4)**: extraction (0x06 + 0x15, file
  + live udp/srt) and insertion (file + live, clock-paced), all on **stock
  gstreamer, no custom element** (the ADR 0008 PMT-rewrite was proven
  unnecessary — [ADR 0015](../context/decisions/0015-no-pmt-rewrite.md)). File
  extraction needs no gstreamer at all ([ADR 0016](../context/decisions/0016-ts-0x15-extraction.md)).
  Design/scope: [`backend-scope.md`](../context/backend-scope.md).
- **Packaging**: compiled `libmisbklv.a` (`find_package(misbklv)` →
  `misbklv::misbklv`) + optional `misbklv-gst` target; **the full CTest suite
  green**; CI runs build/test + the ADR 0012 registry drift check.

## In progress

- Nothing actively mid-change. Next work is pull-based (registry/data breadth as
  samples require it) — see Next.

## Next

- **Registry breadth** (incremental): more 0601 extended items (IMAPB, tags 90+),
  the remaining VTarget items, and 0903 Array types (§9.1.2) as data needs them.
- **Backend follow-ons** (deferred by [ADR 0017](../context/decisions/0017-realtime-streaming.md)):
  a cooperative stop-token to end a live `extract()` early; an SRT-specific
  hermetic test; an RTP payloading option.

## Known gaps

- Need vectors/data: Report-on-Change trimmed packets (samples are all full); a
  real VMTI stream (M3/M5/M6 use hand-authored fixtures — the `data/*.ts` may
  help); multi-byte BER-OID tags (≥128, e.g. Item 143 MSID).
- ST 0102 Security LS (seen as tag 48 in `falls`) is opaque passthrough, not a
  typed nested registry yet.

## Blockers / notes

- Repo directory still `~/workspaces/libklv` (optional `mv` to `libmisbklv`,
  separate from the name decision).
