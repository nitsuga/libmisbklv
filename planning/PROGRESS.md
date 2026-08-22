# libmisbklv — Progress

Current status only — **present tense, volatile, rewrite each session**. The
chronological history (what landed when, milestone/decision detail) lives in
[`../context/log.md`](../context/log.md); the decided-fork register is
[`../context/decisions/index.md`](../context/decisions/index.md); the plan is
[ROADMAP.md](./ROADMAP.md). This file does not re-narrate the past.

## Now

SEI-probe teardown fix on `fix/live-rtp-teardown-probe-uaf-57` (libmisbklv#37, PR #38) — closes the parrot-to-klv#57 use-after-free. `GstInserter::quiesce_to_null()` (from `~GstInserter` and `finish()`) severs the `Generate` SEI pad probes via `remove_sei_probes()` before taking the pipeline to `GST_STATE_NULL` and waiting (bounded) for it, so the streaming thread can't race the `VideoCtx` free; `~VideoCtx` severs idempotently too. Validated before/after under Valgrind memcheck with a mid-stream-teardown harness folded into the suite (pre-fix: 50 invalid accesses into a freed `VideoCtx`; fix: 0). ADR 0024 gained a *Teardown: SEI probe lifetime* section (0020 cross-refs it). Recently landed on `main`: generate-path encoder-shift fix ([ADR 0033](../context/decisions/0033-generate-sei-encoder-timeline-shift.md), #33 — `x264enc`/`avenc_h264` 1000 h `set_min_pts` headroom silently dropped all SEI; matcher now recovers the encoder-adjusted segment), cancellable insert drain ([ADR 0032](../context/decisions/0032-cancellable-insert-drain.md)), and perf fix #27.

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
