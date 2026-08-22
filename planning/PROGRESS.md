# libmisbklv — Progress

Current status only — **present tense, volatile, rewrite each session**. The
chronological history (what landed when, milestone/decision detail) lives in
[`../context/log.md`](../context/log.md); the decided-fork register is
[`../context/decisions/index.md`](../context/decisions/index.md); the plan is
[ROADMAP.md](./ROADMAP.md). This file does not re-narrate the past.

## Now

PR #38 (`fix/live-rtp-teardown-probe-uaf-57`, libmisbklv#37) is validated and ready to merge: it closes the parrot-to-klv#57 SEI-probe use-after-free — `GstInserter::quiesce_to_null()` severs the `Generate` pad probes before the pipeline reaches `GST_STATE_NULL` (detail in [log](../context/log.md); rationale in ADR 0024 § *Teardown: SEI probe lifetime*, cross-referenced from 0020). Both a Valgrind memcheck control and @ox-alpha's stress rig confirm the probe UAF is gone and that variant A sits at the pre-existing baseline flake rate. Remaining work: merge #38, then bump the libmisbklv pin in parrot-to-klv. A separate, still-unattributed heap-corruption residual (glibc `tcache_thread_shutdown` signature during `Generate` live teardown, ~baseline rate) is out of scope for #38 and tracked downstream on parrot-to-klv#57. Recently landed on `main`: generate-path encoder-shift fix ([ADR 0033](../context/decisions/0033-generate-sei-encoder-timeline-shift.md), #33), cancellable insert drain ([ADR 0032](../context/decisions/0032-cancellable-insert-drain.md)), and perf fix #27.

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
