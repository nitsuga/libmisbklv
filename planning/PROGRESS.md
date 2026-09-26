# libmisbklv — Progress

Current status only — **present tense, volatile, rewrite each session**. The
chronological history (what landed when, milestone/decision detail) lives in
[`../context/log.md`](../context/log.md); the decided-fork register is
[`../context/decisions/index.md`](../context/decisions/index.md); the plan is
[ROADMAP.md](./ROADMAP.md). This file does not re-narrate the past.

## Now

No active work. #50 (benchmark) is parked.

<!-- Keep this section about where the WORK is. A sentence that would still be
     true after a month of no work is knowledge, not status: it belongs in a
     context/ concept or an ADR, with at most a pointer here. -->

## Next

- **Registry breadth — 0903 side**: the remaining VMTI/VTarget items, as data
  needs them. (0601 is complete.)
- **Remaining maintenance issue**: #50 benchmarks Generate-SEI end to end through
  `mpegtsmux` after the merged core benchmark closed its other speculative work.
- **Review fork 37** ([ADR 0041](../context/decisions/0041-st1602-nested-registry.md),
  ST 1602 as a typed nested registry) — proposed, awaiting acceptance.
- Candidate *forks* that need a decision first live in [ROADMAP](./ROADMAP.md)'s
  backlog — not enumerated here.

## Known gaps

- Fork PRs never run on the self-hosted runners: approval is a gate, not an
  isolation boundary, so `runs-on` routes them to `ubuntu-latest`. CI pins an
  immutable image tag rather than `:latest`; bump it after **CI image**
  publishes a new one.
- The CI image must carry `git-lfs`: the repo sets `filter=lfs` attributes, so
  `actions/checkout`'s post-checkout hook fails the job without it. It must also
  carry Valgrind: CMake only registers the targeted GStreamer teardown test
  through its invalid-access checker when the executable is present.
- CI's containerized jobs must run as the runner's uid (`--user 1000:1000`).
  The workspace is bind-mounted from the host and shared with native jobs, so a
  root container leaves root-owned files that the next checkout cannot clean --
  which surfaced first as bogus `generated-drift` failures.

- Report-on-Change, multi-byte BER-OID tags, and malformed-input robustness are
  now covered synthetically by `hardening_test` — including `extract_ts_klv`'s
  resync/cap/PES-spanning behavior; still worth a *real*
  RoC-trimmed or multi-byte-tag capture if one turns up (current coverage is
  hand-constructed, not vendor data).
- Need a real VMTI stream (M3/M5/M6 use hand-authored fixtures; the removed
  external corpus was all 0601 and had no tag 74).
- The other embedded Local Sets and the DLP/FLP/VLP packs are still registered
  as opaque `bytes` (ST 0102, tag 48, is now typed — fork 36,
  [ADR 0040](../context/decisions/0040-st0102-nested-registry.md)). Typing them
  is a candidate fork in [ROADMAP](./ROADMAP.md).
