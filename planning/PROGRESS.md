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
- **The ST 0601 registry covers the whole standard** — every §8 item except the
  deprecated Item 66 is typed, so the sample streams decode with no unregistered
  tags. Items 0601 defines as another standard's Local Set or as a DLP/FLP/VLP
  pack are registered `bytes`: named and raw-accessible, byte-identical
  round-trip. Scales are held by `st0601_examples_test`, which runs the
  standard's own per-item worked examples.
- **gstreamer media backend — complete (B0–B5)**: extraction (0x06 + 0x15, file
  + live udp/srt) and insertion (file + live, clock-paced), all on **stock
  gstreamer, no custom element** (the ADR 0008 PMT-rewrite was proven
  unnecessary — [ADR 0015](../context/decisions/0015-no-pmt-rewrite.md)). File
  extraction needs no gstreamer at all ([ADR 0016](../context/decisions/0016-ts-0x15-extraction.md)).
  Design/scope: [`backend-scope.md`](../context/backend-scope.md).
- **Insertion can carry video** ([ADR 0020](../context/decisions/0020-video-passthrough.md)):
  `InsertConfig::video_source` / `KlvSink(sink, realtime, video_source)` re-muxes
  a source file's video elementary stream, parsed but never decoded, alongside
  the KLV — so one call authors a TS with both a video PID and a KLV PID.
  Codec-agnostic (H.264/H.265, TS or MP4 in); the source's audio and its own KLV
  are dropped. The caller must push KLV with real PTS on the video's timeline
  (`kNoPts` is rejected); `realtime` + video is refused as unexercised.
  **An insert session leaves an output file only if it succeeded**
  ([ADR 0022](../context/decisions/0022-no-output-on-failure.md)) — a failing or
  never-called `finish()` removes the file it created, never one that was there
  first.
- **Passthrough video can carry ST 0604 timestamps, on request**
  ([ADR 0024](../context/decisions/0024-sei-generation-opt-in.md) over
  [ADR 0023](../context/decisions/0023-st0604-sei-passthrough.md)):
  `InsertConfig::sei_0604` / a `KlvSink` argument selects `Sei0604::Preserve`
  (**default** — the video ES is not touched and comes out byte-identical) or
  `Sei0604::Generate`. Under Generate the H.264 access unit is rewritten: a
  Precision Time Stamp built from the KLV's item-2 `sensorTimestamp` goes in
  before the first slice, and the KLV becomes the stream's single timestamp
  authority — any ST 0604 the source carried is replaced, Picture Timing SEI
  stripped. Frames are matched by PTS (backward-only, 200 ms); a frame with no
  match gets no SEI rather than an invented one. No transcode either way. H.264
  only — H.265 and reading 0604 back out stay deferred
  ([ADR 0009](../context/decisions/0009-st0604-deferred.md)). NAL and SEI
  parsing go through gstreamer's `codecparsers`; both modes are checked by
  decoding SEI back out of the muxed output. The Time Status says **Lock
  Unknown** (`0x9F`, [`st0603`](../context/st0603.md) §7.4 Table 3) — we relay a
  timestamp whose clock discipline we cannot know, so we don't claim otherwise.
- **Read and write share one timeline**
  ([ADR 0021](../context/decisions/0021-read-path-timestamps.md)): both
  extractors report `pts_ns` as nanoseconds from the start of the source — the
  timeline `push()` writes on — so `KlvStream` → edit → `KlvSink` keeps a stream
  where it was, video branch included. `kNoPts` now means the stream carried no
  timestamp (as `Day Flight.mpg` genuinely doesn't), not that we didn't look.
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
  now **covered synthetically** by `hardening_test`; still worth a *real*
  RoC-trimmed or multi-byte-tag capture if one turns up (current coverage is
  hand-constructed, not vendor data).
- Need a real VMTI stream (M3/M5/M6 use hand-authored fixtures — the `data/*.ts`
  are all 0601, no tag 74).
- ST 0102 Security LS (tag 48) is registered as named opaque `bytes`, not a
  typed nested registry — as are the other embedded Local Sets and the
  DLP/FLP/VLP packs. Typing any of them is a candidate fork in
  [ROADMAP](./ROADMAP.md).
