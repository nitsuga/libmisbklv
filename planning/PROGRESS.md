# libmisbklv — Progress

Current status only — **present tense, volatile, rewrite each session**. The
chronological history (what landed when, milestone/decision detail) lives in
[`../context/log.md`](../context/log.md); the decided-fork register is
[`../context/decisions/index.md`](../context/decisions/index.md); the plan is
[ROADMAP.md](./ROADMAP.md). This file does not re-narrate the past.

## Now

Working through the 2026-08-17 review issues. Issues #4 (live PtsMarks growth),
#6 (gstreamer error-path hygiene), and #7 (core codec/API polish) are done.

<!-- Keep this section about where the WORK is. A sentence that would still be
     true after a month of no work is knowledge, not status: it belongs in a
     context/ concept or an ADR, with at most a pointer here. -->

## In progress

None currently.

## Next

- **Registry breadth — 0903 side**: the remaining VMTI/VTarget items, as data
  needs them. (0601 is complete.)
- **An SRT-specific hermetic streaming test** — the udp live path is covered,
  SRT isn't. Live `extract()` ends on `udpsrc`'s idle-`timeout` message, which
  `srtsrc` has no equivalent for (its properties are `poll-timeout` / `latency` /
  `mode` / `wait-for-connection`), so the test likely has to terminate via the
  ADR 0019 stop token — and may surface a real gap in live SRT extraction.
- Candidate *forks* that need a decision first live in [ROADMAP](./ROADMAP.md)'s
  backlog — not enumerated here.

## Known gaps

- Report-on-Change, multi-byte BER-OID tags, and malformed-input robustness are
  now covered synthetically by `hardening_test` — including `extract_ts_klv`'s
  resync/cap/PES-spanning behavior; still worth a *real*
  RoC-trimmed or multi-byte-tag capture if one turns up (current coverage is
  hand-constructed, not vendor data).
- Need a real VMTI stream (M3/M5/M6 use hand-authored fixtures; the removed
  external corpus was all 0601 and had no tag 74).
- ST 0102 Security LS (tag 48) is registered as named opaque `bytes`, not a
  typed nested registry — as are the other embedded Local Sets and the
  DLP/FLP/VLP packs. Typing any of them is a candidate fork in
  [ROADMAP](./ROADMAP.md).
