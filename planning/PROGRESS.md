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
- **Real-world hardening** done: multi-byte BER-OID tags (≥128), Report-on-Change
  trimmed packets, and adversarial/malformed input (integer-overflow OOB guards in
  the length arithmetic, length validation in `codec::decode`) — all covered by
  `hardening_test`, and the core is clean under ASan+UBSan (`MISBKLV_SANITIZE`).
- **High-level API** done ([ADR 0018](../context/decisions/0018-high-level-api.md)):
  `Message` (owned packet — **parse an existing one or `create` from scratch**,
  typed `get<T>`/`set` by number **or generated `tags::` name**, byte-exact
  `encode`, auto registry by UL key) in the core; `KlvStream`/`KlvSink` (range-for read,
  edit, emit — file + live) in `misbklv-gst`. End-to-end `api_test`, a `klv_edit`
  example, and a user guide ([`docs/api.md`](../docs/api.md)). **Early break from
  a live stream is safe** — `extract` takes a `std::stop_token` and `KlvStream`'s
  destructor cancels promptly ([ADR 0019](../context/decisions/0019-extract-cancellation.md),
  `stop_test`).
- **Packaging** — installable + consumable, verified with real out-of-tree
  builds: `find_package(misbklv)` → `misbklv::misbklv` (core, no gst dep), and
  `find_package(misbklv COMPONENTS gst)` → `misbklv::gst` (the streaming facade;
  its config re-discovers gstreamer). **The full CTest suite green**; CI runs
  build/test, a **consumer smoke test** (find_package, both components), a
  **sanitizer job** (ASan+UBSan core), and the ADR 0012 registry drift check.

## In progress

- Nothing actively mid-change. Next work is pull-based (registry/data breadth as
  samples require it) — see Next.

## Next

- **Registry breadth** (incremental authoring, no new decision): more 0601
  extended items (IMAPB, tags 90+) and the remaining VTarget items, as data
  needs them.
- **An SRT-specific hermetic streaming test** — the udp live path is covered,
  SRT isn't.
- Candidate *forks* that need a decision first live in [ROADMAP](./ROADMAP.md)'s
  backlog — not enumerated here.

## Known gaps

- Report-on-Change, multi-byte BER-OID tags, and malformed-input robustness are
  now **covered synthetically** by `hardening_test`; still worth a *real*
  RoC-trimmed or multi-byte-tag capture if one turns up (current coverage is
  hand-constructed, not vendor data).
- Need a real VMTI stream (M3/M5/M6 use hand-authored fixtures — the `data/*.ts`
  are all 0601, no tag 74).
- ST 0102 Security LS (seen as tag 48 in `falls`) is opaque passthrough, not a
  typed nested registry yet.
