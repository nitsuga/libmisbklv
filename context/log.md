# Knowledge Bundle Log

## 2026-08-30

* **Prepared targeted GStreamer teardown memory checking** (issue #71): the CI
  image definition now includes Valgrind so the existing `teardown_probe_uaf`
  CTest can run under its invalid-access checker. Immutable image publication
  and workflow pinning follow from this exact definition commit. Author:
  openai/gpt-5.6-sol.

* **Closed the remaining bounded-extraction and timestamp-validation gaps**
  (issue #70): `extract_ts_klv` now caps aggregate pending PES bytes before the
  KLV framer runs, releases rejected PID candidates, preserves an exact-cap KLV
  frame plus transport overhead, and reports an incomplete final frame as
  `Truncated` after delivering prior packets. Both GStreamer insertion overloads
  reject timestamps below `kNoPts` while preserving the KLV-only sentinel, and
  the public `Message` docs now spell out duplicate-tag read/edit behavior.
  Focused boundary, multi-PID, tail, and span/vector regressions accompany the
  changes. The full release and core sanitizer CTest suites pass. Author:
  openai/gpt-5.6-sol.

## 2026-08-29

* **CI review follow-up (PR #68, parrot-to-klv#109):** documented that the
  pinned CI image is consumed by parrot-to-klv and that the two-slot publisher
  claim is capacity-bounded but not FIFO-fair.

* **Shared self-hosted CI capacity (issue #67, parrot-to-klv#108, [ADR 0037](./decisions/0037-shared-ci-capacity-gate.md)):** normal jobs claim one of two shared host slots, while the CI image publisher claims both, allowing parrot-to-klv's two `-j3` jobs to run in parallel without relying on manual cross-repository timing.

* **CI moved to self-hosted runners in a container image** (PR #66): three
  runners on one host serve this repo, so jobs run in parallel instead of
  queueing. `ci/Dockerfile` bakes the toolchain into a GHCR image, which
  removes the runtime `apt-get` that made concurrent jobs race on the dpkg
  lock; a shared `/ccache` host volume keeps compilation warm across fresh
  checkouts (measured 23.2s cold, 2.9s warm, 24/25 hits on the sanitize
  preset). Build parallelism is pinned to `-j3` so three concurrent jobs fit
  the host. `generated-drift` stays on the native runner: under `container:`
  git reports every generated file as drift, which did not reproduce outside
  CI on the same image and commit. Review (openai/gpt-5) then closed the
  security gaps: fork PRs route to `ubuntu-latest` because approving a fork is
  a gate rather than an isolation boundary, the image publisher no longer runs
  on `pull_request` and scopes its registry credential to a per-job
  `DOCKER_CONFIG`, and CI pins an immutable commit tag instead of racing
  `:latest`. Author: claude/opus-5.

* **Reconciled documentation and installed-package drift** (issue #64): the
  installed GStreamer config now recreates the codecparsers dependency and the
  consumer smoke test calls the backend factory; corrected timestamp-origin
  wording, current progress/roadmap status, historical backend-plan labels,
  and the amended ADR 0023/0027 claims. Release build, all 28 CTest cases, and
  the installed consumer link check pass. Author: openai/gpt-5.

* **`Error::SourceUnavailable` separates an absent live source from an
  unsupported configuration** ([ADR 0036](./decisions/0036-source-unavailable-error.md),
  fork 33, issue #61). Appended to the enum so existing numeric codes keep their
  values; only the two RTSP sites in `prepare_rtsp_branch` return it, and
  `Unsupported` is now unambiguously permanent. Review caught that the RTSP
  branch's two failure paths each cover both meanings, so they are classified
  rather than assumed: a bus error routes on its `GError` domain
  (`GST_RESOURCE_ERROR` transient, except `NOT_AUTHORIZED`; stream/core and any
  unrecognized domain permanent), and the no-video-pad timeout splits on whether
  `rtspsrc` announced a pad at all — one means the server answered with media
  this build cannot carry, none means nothing answered. `classify_rtsp_error` is
  exposed internally so the permanent paths, which need a live server to reach
  end to end, are covered directly. Review then caught that the branch watches
  the *whole pipeline* bus, so a sink failure — a full disk, an output that
  cannot be opened — arrived as a `GST_RESOURCE_ERROR` and was classified as a
  retryable source outage. Errors are now attributed by origin first
  (`object_within_branch`), with anything outside the RTSP branch reported as
  `Backend`, and `GST_RESOURCE_ERROR` narrowed to its read-side codes as
  redundant defense. A further round added RTSP status awareness: `rtspsrc`
  folds most non-2xx responses onto `RESOURCE/READ` and 404 onto
  `RESOURCE/NOT_FOUND`, so a bad path or an unsupported transport read as an
  absent source; the `rtsp-status-code` detail now decides when present, and
  only a 5xx is transient. Behavior change: an unreachable
  RTSP source reports `SourceUnavailable` where it reported `Unsupported`. Debug
  build clean; all 28 CTest cases pass.


* **Accepted nonblocking terminal status for live insertion**, narrowed to
  failure-only ([ADR 0035](./decisions/0035-live-insert-terminal-status.md),
  fork 32, libmisbklv#58 / PR #59). Clean EOS was dropped from the contract as
  unreachable before `finish()`: `mpegtsmux` is a `GstAggregator` and forwards
  EOS only once every sink pad has it, and the KLV appsrc pad is EOS'd only by
  `finish()` — confirmed by two `gst-launch-1.0` runs on GStreamer 1.24.2 that
  saw no bus EOS in 20s. `poll()` returns `Result<std::monostate>` and pops
  `GST_MESSAGE_ERROR` alone, leaving EOS for the drain. Proposed by
  `openai/gpt-5`; narrowed and accepted by `claude/opus-5` on review. Debug
  build clean; all 28 CTest cases pass.

## 2026-08-28

* **fix: finish promptly after polled EOS** (libmisbklv#58 / PR #59): a
  terminal EOS consumed by `Inserter::poll()` now skips the second drain wait;
  the public sink guide and README describe the live polling contract, and a
  bounded-source regression covers clean close after a polled EOS.

* **Proposed nonblocking terminal status for live insertion** ([ADR 0035](./decisions/0035-live-insert-terminal-status.md), libmisbklv#58 / parrot-to-klv#94).

## 2026-08-27

* **refactor: one private UDP `host:port` parser** (issue #45). Moved the identical bracketed-IPv6 split, bare-IPv6 rejection, and 1–65535 port validation out of the GStreamer source and sink constructors into `detail::parse_host_port`; `make_src` and `make_sink` now share the result while retaining their separate element configuration. The proposed shared video-branch wait loop remains rejected because the file and RTSP paths intentionally differ, and the repeated `VideoCtx` assignments stay visible because a factory would save almost no code. The existing UDP multicast test now directly pins hostname and scoped/bracketed-IPv6 parsing plus bare-IPv6, empty, zero, out-of-range, and trailing-character rejection; the broader extraction, insertion, and live-stream suites exercise both callers. Release build clean; all 28 CTest cases pass, as do the sanitizer core suite's 17.

* **chore: define and apply the C++ formatting baseline.** Added a root `.clang-format` based on LLVM's C++ style with the repository's established differences explicit: 2-space structure with indented access/case labels, left-bound pointers, 100-column lines, two spaces before trailing comments, preserved include order, and one-line guards/loops. Applied it once to every handwritten C++ source in an isolated formatting-only commit and listed that commit in `.git-blame-ignore-revs`; generated registry headers opt out through a nested config because their generator and drift check own their exact bytes. The agent guide uses `git clang-format origin/main` so ordinary changes format only branch-modified lines, with no reason for another whole-tree sweep.

* **refactor: one private KLV framer for both extractors** (issue #48): moved UL resynchronization, partial-prefix retention, bounded frame inspection, timestamp attribution, consumed-prefix erasure, and `PtsMarks` pruning into `detail::KlvFramer`. The gst-free TS extractor and GStreamer appsink path now feed the same implementation; each retains only its own transport setup and error channel, so the GStreamer atomic never leaks into the single-threaded core path. The GStreamer extractor still performs its existing natural-EOS truncation check against the framer's retained bytes, preserving the paths' established end-of-input behavior. The refactor is a net deletion and adds no public API or test: the existing hardening, timestamp-mark, TS extraction, and GStreamer framing suites exercise the shared path from both callers.

* **bench: add `klv_bench`, and let it settle the parked performance questions** (issue #50). A timing harness over the codec, `Message`, `extract_ts_klv`, and gst-extraction paths — median of repeated runs, no dependency beyond `<chrono>`, built by default so it cannot rot, and deliberately **not** a CTest case, since a timing threshold in CI measures the runner rather than the library. The `.ts` fixtures are ~1.5 KiB, so the extraction rows concatenate whole copies to a requested size; which is sound for `extract_ts_klv` and, as review established, not sound for a demuxer — see the withdrawn gst row below. **Building it produced five corrections, none of them from reading the code:** the per-kind codec loop filtered on `!variable`, which excluded all fourteen IMAPB items in the 0601 registry — every one is variable-width with a `fixed_len` default — so the single row the benchmark existed to produce was silently absent; the edited-encode row called `set()` with a value whose type did not match the descriptor kind, failing `TypeMismatch` and skipping without a word; a first write-up said *eighteen* IMAPB items in 0601, which is the count across all three registry tables (14 + 2 + 2) misattributed to one; and the rows were being compared across different widths, which is not a per-kind number at all, since `rd_uint` loops over the bytes. **Widths are now matched at 2 bytes**, the one width UInt, LinearLDS and IMAPB all offer in 0601 (Int has only 1/4/8 and is labeled as differing) — which also disproved an earlier guess that the first row read high from clock ramp-up: it was an 8-byte UInt, and at 2 bytes the same kind drops from ~147–173 ns to ~65–76 ns. **What the numbers say, on one desktop:** at equal width IMAPB decode+encode is ~148–178 ns against ~77–78 ns for LinearLDS, the same-shape control — both decode to a double and re-encode, one with `ceil`/`log2`/`exp2`. So precomputing the ST 1201 scaling factors ([#49](https://github.com/nitsuga/libmisbklv/issues/49)) saves on the order of 70–100 ns per item, and even at the registry's maximum of fourteen such items in a packet at 30 Hz that is ~40 µs per second, well under 0.01% of a core. Worth stressing that the maximum is not the typical: the project's own `synthetic-basic.klv` carries **no** IMAPB items at all. `extract_ts_klv` runs at ~2.1–2.2 GiB/s including both of its passes, so the second sweep costs milliseconds on inputs this library will see. Both are recommendations to *close* rather than act. **A gst-extraction row was written, measured, and then removed** — the fifth correction, and the one that needed an outside reviewer. Concatenating a ~1.5 KiB fixture restarts continuity counters, PCR and PTS at every boundary; `extract_ts_klv` is indifferent (it selects the KLV PID by content and never reads a continuity counter), but a demuxer is not, so the row was measuring recovery from thousands of injected discontinuities. Equal extracted-packet counts across both extractors, which is what the first draft offered as evidence, prove only that the data came back — not that the timing is steady-state demux. Review measured the difference: maintaining continuity counters alone moved gst throughput from 32.6 to 50.2 MiB/s on one machine while the gst-free path held ~2.1 GiB/s. The row and its "some fifty times slower" conclusion are withdrawn. A representative gst number needs a fixture generated at length with coherent CC/PCR/PTS, which belongs in `generate_synthetic_fixtures.py` rather than in the benchmark.

* **fix/docs: close out the two ADR 0032 review findings** (issues #34, #35). **#34 was not the bug it was filed as.** The report had the drain loop's `static_cast<GstClockTime>(remain)` wrapping a negative `remain` into a near-infinite `gst_bus_timed_pop_filtered` poll; that branch is never reached. `remain` is a signed `long long` and `kFinishDrainPoll` an unsigned `GstClockTime`, so `remain > kFinishDrainPoll` converts both to unsigned, a negative `remain` compares *greater*, and the ternary selects the 100 ms cap — confirmed by compiling the expression in isolation (`remain == -1` yields `poll == 100000000`). The real worst case is a bounded overshoot of one poll interval past the deadline, then the loop re-checks and exits. The loop still changed, for legibility rather than correctness: the deadline moved out of the `while` condition entirely and into an explicit `now >= deadline` break, so exactly one clock sample decides each pass and `remain` is positive by construction, leaving the cast independent of any implicit conversion. (A first attempt added the guard while leaving the condition's own `now()` in place, which review caught — it left two samples per pass and made the accompanying comment false.) One behavior change rides along: with the deadline gone from the condition, a pass where the caller has requested a stop *and* the drain has timed out now reports cancellation rather than a drain failure, because the stop check runs first. Both discard a partial sink file (ADR 0022); cancellation returns ok where a timeout does not. Worth noting the repository compiles with no `-Wall`/`-Wextra` outside the sanitizer preset, so the `-Wsign-compare` that flags this expression never fired here. **#35 was correct, and ADR 0032 understated it**: the Consequences bullet read "implementers gain a defaulted parameter — no existing call site changes", which holds for callers and not for overriders, since a default argument does nothing for a class implementing the pure virtual `Inserter::finish()`. The bullet now states the source break, why it is accepted (pre-1.0, no release tags, no external `MediaBackend`/`Inserter` implementation known) rather than absorbed behind a non-virtual forwarding facade, and that it belongs in release notes at the first stability boundary. No decision changed, so the register is untouched. Release build warning-free; all 28 CTest cases pass.

* **cleanup: stdlib replacements and one dead preset** (issues #46, #47): `Registry::find` drops its hand-rolled `lo`/`hi`/`mid` loop for `std::ranges::lower_bound` with a `&ItemDescriptor::tag` projection (same binary search, `constexpr` in C++20, so the generated tables' usage is unaffected), and the two caps-name prefix tests in `gst_video.cpp` use `std::string_view::starts_with` instead of `std::strncmp(..., 6)` behind their existing null guards. `CMakePresets.json` loses the hidden configure preset `base`, which carried a `displayName` and a `description` and no cache variables at all — `release` and `debug` named it in `inherits` while inheriting nothing, so both lost a dangling reference with it. **Two things deliberately not done**, both narrowed out by review: `registry_by_key`'s byte loop stays (a `ranges::equal` replacement needs a projection across the `uint8_t`/`std::byte` mismatch and is not clearly simpler), and `parse_video_source`'s scheme walk stays, because it does not only detect `rtsp`/`rtsps` — it also decides whether an unrecognized `scheme:rest` is `Unsupported` or falls through to the bare-path branch, which is what keeps a Windows `C:\path` reaching `filesystem::exists`. The `default` configure/build preset pair also stays: redundant internally, but a cheap external command alias whose removal could break an undocumented consumer invocation for no gain. Release build warning-free, all 28 CTest cases pass; sanitizer core build passes its 17.

* **fix: `extract_ts_klv` now prunes its timestamp marks** (issue #48): the gst-free extractor and the live extractor's `ExtractCtx::drain()` implement the same framing loop twice — UL resync, 1–3 byte suffix retention, `inspect_packet_frame`, `marks.at()`, erase-and-advance-`stream_off` — and the two had already diverged. `drain()` calls `marks.prune(stream_off)` after erasing consumed or resynchronized bytes; `extract_ts_klv` did not. On a selected KLV PID that carries garbage never completing a frame, TS reassembly stays bounded by resync, but `stream_off` advances with no `on_packet()` call to consume marks through `at()`, so the mark queue grew one entry per PES without limit. The fix is the one `prune()` call the gst path already had. **It is a resource bound only, not a timestamp correction:** `prune()` advances `cur_` exactly as `at()` would, so every delivered `pts_ns` is byte-for-byte what it was before — which is also why no black-box test at the `extract_ts_klv` boundary can distinguish the two versions. The bound itself stays asserted where it is observable, in `pts_marks_test`, and **no test was added at the `extract_ts_klv` boundary**: a first attempt added a sustained non-framing case to `hardening_test`, and review confirmed by deleting the `prune()` call in a scratch worktree that the case still passed — it guarded nothing the existing resync coverage did not already reach, so it was removed rather than kept as apparent protection. Centralizing the duplicated framer is deliberately **not** part of this change: it is worth doing only if a shared type removes both loops without dragging the gst side's atomic error plumbing into the single-threaded TS path, and that is a design question, not a bug fix. Release build clean; all 28 CTest cases pass.

## 2026-08-25

* **docs: `references/` now records where the standards came from and on what terms.** The pre-public audit found the six MISB standards deposited here — ST 0107.5, ST 0601.19, ST 0603.5, ST 0604.6, ST 0903.6, ST 1201.5, each a publisher PDF plus a `.txt` extract — carried **no recorded provenance at all**: no source URL anywhere in the repository, no terms note, no `references/README.md`, and no ADR. That is the same gap [ADR 0016](https://github.com/nitsuga/parrot-to-klv/blob/main/context/decisions/0016-references-licensing-withdrawal.md) had just closed in the sibling parrot-to-klv, where the answer was a withdrawal. Here it is not: MISB is a board of the NGA, its standards are U.S. Federal Government works, and under 17 U.S.C. § 105 copyright is unavailable for such works, placing them in the public domain in the United States — corroborated by Wikimedia Commons hosting MISB ST 0601.8 under `PD-USGov` sourced from `gwg.nga.mil`. The absence of a copyright notice or distribution statement on the documents is consistent with that, not evidence against it. **Withdrawing the PDFs was considered and rejected twice over:** dropping the PDFs while keeping the `.txt` extracts would remove one container of a work and leave the same text in the other (`ST0601.19.txt` is 573 KB of the standard's own words), which is incoherent; and dropping both would delete the normative source of a KLV library, break 31 citations across 11 documents, and cost the grep/Read workflow CONVENTIONS is built on — all on an unverified suspicion, with no permissively-licensed replacement to vendor in the way Parrot's BSD-3 `vmeta.proto` replaced its documentation. The new `references/README.md` records the table, the § 105 basis, and the two things **not** established: NGA's own endpoints were unreachable at the time of writing (`nsgreg.nga.mil` no response, `gwg.nga.mil` HTTP 403), so nothing here is confirmed from the publisher; and § 105 is a U.S.-only rule.

* **Correction to yesterday's security-rule entry:** it stated that reaching the removed third-party media in history "would have required a `filter-repo` rewrite invalidating every SHA here". Checked during the same audit, that is false for this repository — the corpus was never committed. Scanning `git rev-list --objects --all` across every branch, all tags and all 23 fetched `refs/pull/*` refs finds zero `.mp4`/`.mov`/`.mpg`/`.mpeg`/`.avi`/`.7z` blobs and no trace of `Day Flight.mpg`, `Night Flight IR.mpg`, `Cheyenne.ts`, `falls.ts` or `klv_metadata_test_sync.ts`; the only path ever committed under `data/` is `README.md`. The removal recorded in [`data-samples.md`](./data-samples.md) took effect in the working tree, and this repository's history is already clean of it. No rewrite is owed here. The pending rewrite in parrot-to-klv, whose history does carry withdrawn blobs, is unaffected.

## 2026-08-24

* **docs: adopt the shared security rule on location and identity data**: `AGENTS.md` had **no** security rule, while the sibling parrot-to-klv carried one naming "embedded GPS coordinates" — two repositories holding different standards for the same class of data, found during the pre-public audit of this repo. Added here as its own section, widened on both sides to what actually identifies a recording (coordinates, platform or sensor identity, tail numbers, absolute times) across code, tests, fixtures, commit messages, documentation and agent memory, with `test/fixtures/` stated as synthesized by `generate_synthetic_fixtures.py` rather than derived from supplied media. **The rule is scoped to media this project holds and anything derived from it.** A first draft was broad enough to cover published facts about third-party media the project does not hold, which made [`data-samples.md`](./data-samples.md) an apparent violation and required a carve-out to exempt it; that shape was rejected, because the carve-out only undid an overreach one step earlier. Correctly scoped, bibliography was never in scope. Redacting `data-samples.md` was considered and rejected on its merits: it describes third-party public sample files this repository never contained (its coordinates resolve to landmarks, its tail number is public registry data), and it is the artifact showing the corpus was identified and removed on purpose under [ADR 0028](./decisions/0028-hermetic-synthetic-fixtures.md) — stripping its evidence would leave an unsupported assertion in place of a defensible record. Reaching the copies in history would have required a `filter-repo` rewrite invalidating every SHA here, including the `GIT_TAG` pin parrot-to-klv builds against. No ADR: this follows the agent-authorship sync earlier today, where a cross-repo working-rule alignment landed as a log entry rather than a decision.

* **docs: loosen the authorship spelling to `<producer>/<model>`**: the rule added earlier today named the OKF `<producer>/<version>` form specifically, which fits `claude/opus-5` but forces a harness/model string like `opencode-go/muse-spark-1.2-contributor` to be split by guesswork. The slot now takes the vendor *or* harness plus the model — one pair, no exceptions, still identical to what `generated.by` carries. Syncs this file with the sibling parrot-to-klv `AGENTS.md`, where the same question came up against agents that actually post there; both copies now say the same thing.

* **fix: silence the spurious `-Wstringop-overflow` in `extract_ts_klv`** (issue #41): GCC 12/13 lose the iterator bounds when inlining `vector::_M_range_insert`'s reallocation path and invent a write of "between 2 and SIZE_MAX bytes into a region of size 0" at the PES continuation append. Neither half is reachable — `payload` is bounded to [0, 184] by the `off > i + kPkt` guard and the destination is non-empty by the enclosing condition — so the fix is a narrowly scoped `#pragma GCC diagnostic` around that one statement, guarded to GCC (clang does not know the option), with the reasoning in a comment for re-evaluation when the toolchain moves. Chose suppression over rewriting the append: the code is correct, and reshaping it to appease a compiler bug would have been the more invasive change. Release build is now warning-free; all 28 CTest cases pass.

* **docs: CONVENTIONS no longer claims a single bundle actor**: the Actors paragraph asserted every `generated.by` here is `claude/opus-5`, which ADR 0034's `openai/gpt-5` had already falsified. It now says the field names whichever agent authored that doc and points at the new AGENTS.md attribution rule. Existing PR #40 comments and issue #41 were re-signed from `anthropic/claude-opus-5` to the bundle's `claude/opus-5` spelling.

* **docs: require agent self-attribution on PRs, issues, and comments**: ported the rule from the sibling parrot-to-klv `AGENTS.md` now that more than one model posts here (PR #40 carried both `openai/gpt-5` and `claude/opus-5` prose). An agent names itself as the author in the PR/issue/comment body, separate from the git commit author, spelled with the bundle's existing `<producer>/<version>` actor convention rather than parrot-to-klv's `<provider>/<model>` — one spelling per repo.

* **fix: bound live EOS on the contended mux sink lock (PR #40 re-review)**: the first lock-bound follow-up polled the branch ghost source pad, but chain-driven live branches can leave that lock idle while `mpegtsmux` holds its reserved sink pad lock; `gst_pad_push_event` could therefore still enter an unbounded implicit wait. `push_live_eos_when_idle` now derives the source peer from `VideoCtx`, acquires the source and `reserved_video_pad` stream locks in source-to-sink order, then pushes while holding both recursive locks. A hermetic lock-contention regression holds the mux-side pad lock while proving the ghost lock remains free and EOS returns at its injected acquisition deadline without entering the handler; restoring the wrong-pad implementation makes all three assertions fail. ADR 0034 now states that the bound ends at handler entry and explicitly accepts discarding file output on mux-lock timeout because no clean EOS/drain was established. Historical log entries unchanged. Validation: all 28 release tests pass, the final live-video test passes under allocator perturbation, internal links resolve, and `git diff --check` is clean.

* **fix: remember live-video delivery across close-time stalls; bound serialized EOS lock acquisition (PR #40 review follow-up)**: the first ADR 0034 implementation armed a temporary buffer probe inside `finish()` and required a new buffer within one second on every unbounded-live close. A healthy RTSP session stalled at that instant therefore failed and deleted its output despite having delivered video for minutes. `VideoCtx` now records first delivery persistently from the reserved mux pad; the one-second caps/buffer gate runs only until that latch is set. EOS still stays serialized and the request pad stays linked, but `finish()` polls `GST_PAD_STREAM_TRYLOCK` for at most five seconds with stop-token checks before pushing, so it cannot wait indefinitely behind a stuck streaming thread. The probe registry was generalized (`register_probe`/`remove_probes`) so the delivery tracker gets the same deterministic raw-`VideoCtx*` lifetime as the SEI probes. Removed the now-write-only `is_live`, simplified dead finish-loop conditions, and corrected the output-cleanup comment for failed NULL confirmation. `live_video_test` now covers both sides of readiness: a previously healthy branch closing during a >1 s video stall preserves non-empty output, while a branch that never delivers fails after the bounded first-buffer wait and leaves no file. A one-shot test sink also proves that failed NULL teardown overrides pre-requested cancellation, reports `Error::Backend`, and removes output without preventing clean destructor disposal. ADRs 0024/0034 amended; historical log entries unchanged. Validation: all 28 release tests pass; five allocator-perturbed `live_video` cycles pass; the Valgrind-backed teardown harness passes all eight of its iterations with zero errors under allocator perturbation.

* **docs: synchronize the landed live-video surface across the public guide, headers, and durable decisions**: removed the remaining file-only/realtime-rejected descriptions from `docs/api.md`, `stream.hpp`, and `backend.hpp`; documented `rtsp[s]:`/`pipeline:` video sources, file-vs-live PTS domains, multicast controls, and cancellable `close(stop)`. ADR 0020 now explicitly marks its original file-only/realtime restriction as superseded by ADR 0031; ADR 0031 records its landed open bounds, running-time/test contract, and ADR 0034 teardown follow-on. `backend-scope.md` now carries those live extensions alongside the original B5 file branch. Historical log entries remain unchanged snapshots.

* **fix: live request-pad teardown drains without a mid-PLAYING release (ADR 0034, fork 31 — #39 / PR #40)**: traced the residual parrot-to-klv#57 heap corruption to `GstInserter::finish()` unlinking and releasing an unbounded live video's `mpegtsmux` request pad while the mux streaming thread could still traverse its pad list. The production fix keeps the pad linked until NULL pipeline destruction, waits briefly for an immediately-closed live branch to negotiate and deliver data, pushes EOS through that pad, and uses the common error-aware five-minute drain. It removes the diagnostic branch's two-second global timeout, success-without-drain path, and unbounded NULL wait; NULL confirmation is bounded, cancellation stays prompt, and incomplete/error output is removed. `live_video_test` now requires non-empty, byte-exact KLV after unbounded-live drain and covers pre-requested cancellation leaving no output. The temporary root `FIX.md`/`NOTE.md` investigation files were removed; durable rationale lives in [ADR 0034](./decisions/0034-live-request-pad-teardown.md). Validation: all 28 release tests pass; five allocator-perturbed repeats each of `live_video` and the Valgrind-backed `teardown_probe_uaf` harness also pass; GitHub release build/test, sanitizer, generated-drift, and link checks are green.

## 2026-08-22

* **build: CMakePresets.json v6→v3; wire ci.yml to `cmake --preset`**: downgraded `CMakePresets.json` from schema v6 (`cmakeMinimumRequired` 3.25) to v3 (3.21) to stay in lockstep with parrot-to-klv, whose `release.yml` pins `ubuntu-22.04` and only has apt `cmake` 3.22.1 — too old for v6. `ci.yml`'s `build-test` job now configures/builds/tests/installs via the `release` preset (`build/release`) instead of ad hoc `-B build`, and `sanitizers` now uses the `sanitize` preset (`build/sanitize`) instead of `-B build-san`. README.md and AGENTS.md updated to match, dropping the now-stale "maps to CI build-san" parenthetical since CI uses the preset directly.

* **build: CMakePresets.json v6 — release/debug/sanitize under build/\*, jobs=6 via hidden base**: added `CMakePresets.json` (version 6, `cmakeMinimumRequired` 3.25): hidden `base` + `release`/`default` → `build/release` (Release, `MISBKLV_GSTREAMER=ON`), `debug` → `build/debug` (Debug, `MISBKLV_GSTREAMER=ON`), `sanitize` → `build/sanitize` (Debug, `MISBKLV_SANITIZE=ON` + `MISBKLV_GSTREAMER=OFF` core-only, maps to `ci.yml` `build-san`). Build presets use hidden `base` with `jobs=6` (`nproc 7 → 6`, `nproc-1` headroom) inherited DRY.

* **fix: deterministic SEI-probe teardown; close parrot-to-klv#57 use-after-free** (libmisbklv#37, PR #38): `Generate` attaches two pad probes (`on_sei_event_probe`, `on_h264_buffer_inject_sei`) that hold a raw `VideoCtx*` and run on the streaming thread; teardown previously never removed them and freed `VideoCtx` relying on `set_state(NULL)` having quiesced those threads — which it need not have, since NULL can return `GST_STATE_CHANGE_ASYNC`. Added a mutex-guarded probe registry on `VideoCtx` (`register_sei_probe`/`remove_sei_probes` + `probes_severed` latch so a late live pad-added can't re-arm the race), and `GstInserter::quiesce_to_null()` — called from both `~GstInserter` and `finish()` — which severs the probes (`gst_pad_remove_probe` blocks on any in-flight callback, so after it no callback can touch the freed ctx) and then `set_state(NULL)`. `~VideoCtx` also severs idempotently (belt-and-suspenders for any path that skips GstInserter teardown). The probe severing alone is what closes the UAF; an earlier revision of this PR also *waited* on `gst_element_get_state` after NULL, but a stress-rig bisect (@ox-alpha) showed the wait is unnecessary for #57 (severing already satisfies the memcheck control) — so it was dropped, restoring `main`'s teardown timing. Framing kept precise: the bisect justifies dropping the wait, it does **not** establish the wait causes flakes. Because we no longer wait, a **non-probe** streaming-thread access after an async NULL (e.g. a late pad-added) remains a known, pre-existing residual — it predates this fix and is tracked on parrot-to-klv#57, not closed here; the ADR does not claim NULL is reached before the free. Validated with a reproduce-first before/after harness against the public backend API that destroys the Inserter mid-stream (live H.264, `Sei0604::Generate`, leaky queue so a probe is in flight at the free): pre-fix code under Valgrind memcheck showed 50 invalid reads/writes into a freed 272-byte `VideoCtx` from both probe callbacks; the fix shows 0. The `live_rtp` CI flake never faulted under ASAN because that suite always tears down after EOS/drain (no probe in flight); the mid-stream harness is what exposed it, and it is folded into the suite as `teardown_probe_uaf_test` (runs under `valgrind` when present, else bare; skips loudly naming `openh264enc`/plugins-bad when no H.264 encoder is present). Rebased onto `main` (keeps #32 cancellable drain and #36/ADR 0033 encoder-shift matcher — the event probe is `on_sei_event_probe` with SEGMENT handling). Full ctest green. ADR 0024 amended with a *Teardown: SEI probe lifetime* section; ADR 0020 cross-references it. parrot-to-klv needs only a libmisbklv pin bump — no code change.

## 2026-08-21

* **fix: Generate SEI matching survives the encoder DTS-headroom timeline shift (ADR 0033, fork 30 — #33)**: `x264enc` and `avenc_h264` move output onto a 1000-hour minimum (`gst_video_encoder_set_min_pts(GST_SECOND*60*60*1000)`, unconditional since GStreamer 1.6; `openh264enc` does not opt in), so under `Sei0604::Generate` every frame missed the KLV PTS map by ~1000 h, injected 0 SEI, and the consume-side eviction then wiped every key on frame one — silently, and only on machines with plugins-ugly installed (CI never exercised x264enc). The matcher now detects the shift: a direct miss beyond a 10 s gate retries at the downstream TIME segment's running time under the same backward-only 200 ms window, latches that space for the segment on success (match and eviction both use the translated key), and leaves agreeing timelines byte-for-byte unchanged. This uses GStreamer's actual adjustment (`min_pts − first_input_pts`) rather than assuming a fixed 1000-hour offset, so non-zero source starts work too; a new segment re-detects. Unconditional running-time matching remains rejected because the working openh264 path can expose a rewritten segment, but the guarded fallback avoids that case. The hermetic lagging-video test starts at a non-zero PTS and runs once per available encoder asserting 90/90 SEI each (was: first pick only); a present encoder's open failure is now a failure, and CI installs plugins-ugly to close the blind spot. All CTest green including `generate_path`, which passes for the first time on x264enc machines.

* **fix: cancellable insert drain — `finish()`/`close()` take a stop token (ADR 0032, fork 29)**: `Inserter::finish()` sent EOS then blocked in its bus-drain loop until pipeline EOS / `ERROR` / the 5-minute `kFinishDrainTimeout`, checking nothing else. For a realtime file replay (`realtime=true` + `file:` video source, ADR 0031) the video drains at wall-clock speed, so once the caller pushed its last KLV packet `finish()` was uninterruptible for the remaining real video duration — a downstream `parrot-to-klv -i file.mp4 -o udp://…` ignored Ctrl-C mid-replay until EOS. Threaded `std::stop_token` through `Inserter::finish(std::stop_token = {})` and `KlvSink::close(std::stop_token = {})` (default token never signals — every existing caller unchanged), mirroring the read-path cancellation of ADR 0019: `GstInserter::finish` caps each drain poll at 100 ms (`kFinishDrainPoll`, down from 1 s) and checks `stop.stop_requested()` each pass; on request it breaks, `set_state(NULL)`, discards any partial sink file (ADR 0022), and returns **ok** (cooperative cancel is a caller request, not a fault — matches `extract()`). `MockInserter::finish` takes and ignores the token. New `gst_video_insert_test` case "realtime drain cancellation": realtime replay of the 2 s TS source, one KLV pushed, `finish()` with an already-requested stop returns in 0 ms (vs ~2 s full drain) and leaves no output file. Interface files touched: `backend.hpp`, `mock_backend.hpp`, `stream.hpp`; impl `gst_insert.cpp`, `stream.cpp`. Pre-existing `generate_path` failure on this machine (OpenH264 SEI-gen, environment) is unrelated and reproduces on clean `main`. Consumer (`parrot-to-klv`) will bump the pin and wire its SIGINT flag to a `stop_source` in a follow-up.

* **perf: redundant packet copies in KlvStream/KlvSink read-write pipeline (#27)**: eliminated two per-packet copies on the read-edit-write hot path. `KlvStream::pull` now uses `Message::adopt(std::vector<std::byte>&&)` to move the already-owned frame bytes into the Message without copying (fixes the `Message::parse` double copy). `KlvSink::emit` takes a fast path for unedited messages via `original_bytes()`/`edited()` bypassing `encode()`, and `GstInserter::push(std::vector<std::byte>&&)` wraps the encoded vector's storage with `gst_buffer_new_wrapped` (destroy notify frees the vector) to avoid the `gst_buffer_fill` copy. `Inserter::push` gains an ownership-transferring overload defaulting to the span path for mock/other backends. All CTest cases green; passthrough remains byte-exact.

* **fix: consume-side timestamp-map eviction; prime codec latch (#26 review)**: moved `pts_to_sensor_timestamp` eviction from producer (`record_sensor_timestamp` 1 s window) to consumer (SEI injection `upper_bound` path erases keys < frame_pts − 200 ms). Producer prune removed entirely; comment now states the map is bounded by actual KLV-vs-video lead (tolerance window) and that Generate with KLV far ahead and no video accumulates until consumed — inherent. Factored duplicated H.264 caps checks into `caps_codec`/`latch_codec_from_caps` helpers and prime `codec_latch` from already-negotiated caps in pipeline, file, and RTSP attach sites so the buffer probe does not stay `Unknown` when no further CAPS event arrives. Updated `generate_path_test`: monotonic test now asserts boundedness driven by consumption (interleaved push+consume stays bounded, and a far-ahead burst grows until consumed), and added hermetic lagging-video regression — map-level (>1 s lead still matched) and pipeline (`videotestsrc num-buffers=90 ! h264parse` into `KlvSink` Generate with bursty 90-packet push) asserting every frame receives its ST 0604 SEI (90/90; old producer prune produced 36/90). Live tallies removed from `planning/PROGRESS.md`. All CTest cases green.

* **perf: Generate-mode video path — latch SEI codec probe, bound timestamp map (#26)**: latched H.264 codec via atomic `CodecLatch` on a downstream CAPS event probe (`GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM`, `gst_structure_has_name` check) so the 30-60 fps SEI buffer probe fast-paths without per-frame `gst_pad_get_current_caps` + string churn; Unknown falls back to per-buffer caps query to keep un-negotiated-pad defense, and renegotiation re-latches. Pruned `pts_to_sensor_timestamp` to a 1 s sliding window (5× the 200 ms match tolerance) after each `record_sensor_timestamp` insert, bounding the map to ~30 entries at 30 Hz instead of growing without bound (~200 MB/24 h); relies on the documented monotonic-push contract, with out-of-order pushes losing accidental backward matches. Shared `attach_generate_probes` helper keeps file/RTSP/pipeline attach sites in sync; ADR 0024 behavior preserved. New `generate_path_test` bounds the map over 10k monotonic pushes. All CTest cases green.

* **perf: zero-copy tag-2 read in record_sensor_timestamp (#25)**: replaced `Message::parse` heap allocation and full payload copy with `parse_packet` borrowed spans and `codec::decode` for tag 2, mirroring `Message::get<std::uint64_t>(2)` semantics exactly (registry lookup via `registry_by_key`, length validation, kind check). Live insert path now avoids per-packet copy. 26/26 CTest green.

## 2026-08-20

* **Wire Generate probe for live branches (RTSP and pipeline)**: `prepare_rtsp_branch` and `prepare_pipeline_branch` no longer reject `Sei0604::Generate` for live sources; RTSP now attaches the same `h264parse` SEI probe (`on_h264_buffer_inject_sei` on `h264parse` src) that the file path uses, and pipeline live attaches the probe to the bin's ghost src pad so H.264 live also carries ST 0604. `prepare_video_branch` early Generate rejection for live removed. `live_video_test` updated: pipeline Generate now succeeds with push/finish, RTSP Generate unreachable still returns Unsupported but via 5 s network path not fast reject. 26/26 CTest green.

* **Live video source and realtime lift (ADR 0031, fork 28, part 2 of 2)**: `video_source` now accepts `file:`/bare path (file via filesrc ! demuxer ! parser, sniff ADR 0025), `rtsp[s]://` (live via rtspsrc latency 0 protocols 0x07 with dynamic rtph264depay/h265 ! parse ! queue linked on pad-added), and `pipeline:<desc>` (explicit GstBin via gst_parse_bin_from_description with ghost src pad). `realtime+video_source` no longer rejected: file replays on pipeline clock, live is normal mode; kNoPts still rejected with video. File branch keeps 10 s PAUSED preroll; live skips PAUSED wait and goes PLAYING with 5 s async pad watch (bus ERROR or timeout → Unsupported) and NULL cleanup on failure. `make_sink` still maps multicast knobs. New `live_video_test` (26/26 CTest) verifies file+realtime, pipeline live via file sink, error cases (pipeline:invalid, http://, rtsp unreachable within 5 s, bare missing), empty→KLV-only, and hermetic UDP loopback with pipeline live video (videotestsrc is-live ! openh264/x265 ! mpegtsmux ! udpsink loopback, KLV byte-exact, 5× green).

* **udp multicast knobs landed (ADR 0031, fork 28, part 1 of 2)**: `InsertConfig` now has `udp_ttl_mcast` (default 1), `udp_mcast_iface` (default ""), and `udp_loop` (default true) mapped to `udpsink` `ttl-mc`/`multicast-iface`/`loop` (+ `auto-multicast` TRUE). Defaults reproduce today's `udp:127.0.0.1:port` behavior; `make_sink` validates TTL 0–255 and correctly handles bracketed `udp:[<IPv6>]:port`. `file:`/`srt:` ignored. New `udp_multicast_test` (25/25 CTest, 10×) verifies the mapping; existing `gst_stream` loopback still green.

* **Accepted the vendor-neutral live streaming surface** (fork 28 —
  [ADR 0031](./decisions/0031-live-streaming-surface.md)): decision is now
  `accepted` and the decided register updated; ROADMAP no longer lists an open
  fork. Implementation (live video source, multicast knobs, realtime + video)
  is now unblocked.
* **Proposed a vendor-neutral live streaming surface** (fork 28 —
  [ADR 0031](./decisions/0031-live-streaming-surface.md)): `video_source`
  accepts a GStreamer URI/description (file, `rtsp[s]:`, `pipeline:`), `udp:`
  sinks grow multicast/broadcast knobs, and `realtime` + a video source stops
  being rejected. Vendor metadata extraction (Parrot/DJI RTP) deliberately stays
  out of the library.

## 2026-08-17

* **Bound live-path `PtsMarks` growth on a non-framing feed** (issue #4):
  `gst_extract.cpp`'s `drain()` only consumed `PtsMarks` entries (`marks.at()`)
  when a complete KLV frame was emitted, but resync past unmatched bytes already
  advances `stream_off` even when no frame ever completes — so a live UDP/SRT
  feed whose KLV PID never frames (wrong port, mislabeled stream) grew `marks_`
  by one entry per appsink buffer forever, unlike the reassembly byte buffer
  ADR 0026 already bounds. Added `PtsMarks::prune(stream_off)`, called from
  `drain()` right after `stream_off` advances, dropping entries strictly behind
  it the same way `at()` retires them, so the queue is bounded by the same
  window as the bytes. New `pts_marks_test` (`pts_marks` CTest case) covers
  `prune` directly — offsets ahead/behind/equal to `stream_off`, an empty
  queue, and sustained non-framing input staying bounded. A second new case,
  `gst_stream_nonframing` (`gst_stream_nonframing_test`), drives the fix
  through a real live pipeline (appsrc ! mpegtsmux ! udpsink over loopback
  UDP): many buffers of UL-free filler on the KLV PID, asserting `extract()`
  still ends cleanly on the udpsrc idle timeout having framed nothing —
  `Inserter::push()` puts arbitrary bytes on the wire, so no lower-level
  plumbing was needed to drive this case after all. Full CTest green.
* **gstreamer backend error-path hygiene sweep** (issue #6): four localized
  fixes on the media backend's failure paths. (1) `gst_video.cpp`'s link-failure
  `g_warning` leaked the demuxer element — `gst_pad_get_parent_element` returns a
  full reference that is now held in a local and unref'd. (2) `on_pad_added`
  (`gst_extract.cpp`) guards NULL/empty caps before `gst_structure_get_name`,
  mirroring the sibling `on_video_pad_added`, so a pad with no caps can't
  NULL-deref. (3) `open_insert` (`gst_insert.cpp`) probes sink pre-existence with
  `std::filesystem::exists` instead of `fopen(path, "rb")`: a pre-existing
  *write-only* sink was misclassified as "created by us" and deleted on the
  failure path, violating the ADR 0022 "a file already at that path is the
  caller's" guarantee. (4) Dropped the dead `sei_codec_unsupported` flag (set,
  never read — the failure is already surfaced by the `g_warning` and a downstream
  `Error::Unsupported`). New `gst_video_insert_test` case pins the write-only-sink
  scenario (verified to fail under the old probe); full CTest green. A follow-up
  makes the ownership probe fail-safe to the letter of ADR 0022: a *stat* error
  (e.g. a permission-denied path component) now counts as pre-existing too —
  only a clean "does not exist" makes the sink removable on the failure path.
* **Core codec/API polish** (issue #7): five review-found hardening items landed
  together. (1) `imapb_encode`/`imapb_decode` now reject a degenerate
  caller-supplied descriptor (`min >= max`) before the IMAPB scale math — encode
  emits the +QNaN special, decode returns NaN — closing a `floor(NaN)`→int UB on
  the public codec surface; the float→int cast is also clamped for defensive
  parity with `linear_encode`. (2) Public `rd_uint`/`imapb_decode` clamp a >8-byte
  span to its low 8 bytes (a defined value, not an incidental wrap), with the
  1..8-byte contract now documented in `codec.hpp`. (3) `Message::encode()`
  enforces mandatory items on the `create()` authoring path (detected by empty
  source bytes) — a created 0601 packet missing tag 2 or tag 65 now returns
  `Error::MissingMandatory` instead of silently emitting a non-conformant packet;
  the parse-then-edit path stays lenient so re-encoding a Report-on-Change capture
  after an edit still works. (4) `Message::set(1, …)` (checksum) is rejected with
  the new `Error::ReadOnly` on both paths, replacing the old split behavior
  (dropped when parsed, emitted twice when created). New/updated cases in
  `imapb_test`, `message_test`, and `hardening_test`; full CTest and the core
  ASan/UBSan suite green.
* **Non-minimal BER long-form length is accepted on read** (fork 27, issue #7
  item 5): decided and documented rather than tightened —
  [ADR 0030](./decisions/0030-non-minimal-ber-length.md).

* **The `-Wall -Wextra -Wpedantic -Werror` build is green again** (issue #8):
  three tests used partial `InsertConfig` aggregate initializers
  (`mock_backend_test`, `gst_insert_test`, `gst_stream_test`), tripping
  `-Wmissing-field-initializers` once video passthrough added `video_source`
  and `sei_0604` (ADR 0020). All now pass the full field list, matching the
  `gst_video_insert_test` call sites. (GCC warns on partial designated
  initializers too, so the fix is full aggregate init, not `{.sink = ...}`.)
  Full CTest green under `-Werror` and in the normal build.

* **ST 0903 variable-length uints get a default encode width** (fork 26, issue
  #5): VMTI tags 4/5/6 and VTarget tag 1 are registered at their standard
  maximum width (`V2`/`V3`/`V6`) with `variable = true`, mirroring the 0601
  variable-uint precedent — making `Message::set(tag, value)` author them
  without a width argument. Rationale in
  [ADR 0029](./decisions/0029-st0903-variable-uint-default-width.md).
  Implemented and pinned: regenerated tables (drift-free), authoring and
  Vmax-boundary tests in `message_test` and `vtarget_roundtrip_test`, and the
  `fixed_len` "default encode width" semantics corrected in `types.hpp`. All
  CTest cases green.

* **`extract_ts_klv` now matches the gstreamer extractor's malformed-input
  robustness** (issue #3): the gst-free batch extractor reassembles KLV packets
  across PES boundaries, resyncs over injected garbage, enforces the shared
  16 MiB reassembly cap (`kDefaultMaxKlvPacketBytes`), and fails terminally
  with `BadLength` / `ResourceLimit` instead of stalling on a corrupt frame —
  packets before the failure stay delivered. `ts.hpp` documents the new
  semantics; `hardening_test` pins corrupt-length, over-cap, resync, PES-split,
  and 0x15 AU-cell-split cases. Full CTest and the core ASan/UBSan suite green.

## 2026-08-12

* **The insert path's output PMT now announces video first, KLV second**,
  reversing the stream-ordering limitation ADR 0020 had recorded as standing —
  [ADR 0020](./decisions/0020-video-passthrough.md) § Stream order (revised)
  owns the mechanism and why the two 2026-07-28 attempts failed.
  `gst_video_insert_test` pins the order; the full battery passes (TS and MP4
  sources, both SEI modes, Time Status, H.265/MPEG-1/2, read→edit→write), and
  a consumer `ffmpeg -map 0:0` selects the video. PROGRESS's known gap and
  `docs/api.md`'s select-by-index caveat are gone.

## 2026-08-10

* **README build requirements now complete.** The gst facade's CMake check
  needs `pkg-config` and `gstreamer-codecparsers-1.0`, but the requirements
  prose and the Ubuntu/Debian apt command listed neither (codecparsers ships in
  `libgstreamer-plugins-bad1.0-dev`). Added both, plus the codecparsers dev
  files and `pkg-config` to the Fedora and Arch package lists. Surfaced while
  wiring up parrot-to-klv's own README after a bare host's mux test failed with
  "media pipeline failed".

## 2026-08-08

* **Fork-free test-data provenance decision accepted:** third-party media and
  derived fixtures are removed from the current tree; backend regression inputs
  move to project-authored deterministic fixtures, and `data/` remains an ignored
  workspace for developer-provided media. The small generated fixtures are
  committed and CI-checked against their Python 3.11+ recipe, so normal source
  and installed-library builds need no Python
  ([ADR 0028](./decisions/0028-hermetic-synthetic-fixtures.md)). Fresh final
  verification passed all 16 core-only cases with Python discovery disabled,
  all 22 GStreamer cases, and all 16 core cases under AddressSanitizer and
  UndefinedBehaviorSanitizer.

* **Rewrote repository history to withdraw the external corpus:** the five LFS
  media paths and seven derived KLV fixtures are absent from reachable commits;
  the local LFS payload cache was pruned and GitHub `main` was force-updated.
  Host-retained cached commits and orphaned LFS payloads were reported for the
  separate GitHub Support purge.

* **Corrected the historical-corpus attribution** in
  [`data-samples`](./data-samples.md). The three `.ts` files shared the QGISFMV
  distribution channel but not a rightsholder, so attributing all of them to Esri
  was wrong: only `Cheyenne.ts` is Esri (Mission ID `ESRI_Metadata_Collect`, tail
  `N97826`, C208B over Cheyenne, Wyoming), `falls.ts` is an L3 WESCAM MX turret
  capture over Snoqualmie Falls with vendor-private `WESCAM`/`ARSX`/`JSONCMD`
  streams and no locatable terms, and `klv_metadata_test_sync.ts` carries
  injector-generated KLV using ST 0601's own printed example values (`Predator`,
  `EO Nose`). Each identification comes from the files' own ST 0601 items. The
  evidence is now tabulated there because it dies with the media otherwise, and
  `falls.ts` — the separate rightsholder — is the one the purge most needs to
  cover.

## 2026-08-01

* **Migrated the knowledge bundle to OKF v0.2.** `timestamp:` became
  `generated: {by, at}` across every concept and ADR, carrying each doc's
  existing timestamp through unchanged; the actor is `claude/opus-5` throughout,
  never the `human:` prefix, because trust tooling keys off that prefix and
  these are agent-written docs. ADRs carry `decision_status:` rather than
  `status:`, since v0.2 claims `status` for *document* lifecycle
  (draft/stable/deprecated) — a different axis from decision state, as a
  superseded ADR is still a stable document. CONVENTIONS gained the v0.2
  frontmatter shape, the actor convention, the optional-families table, and a
  `# Citations and sources` section giving `sources:` and the body `# Citations`
  separate jobs: the machine-readable index of external inputs, and the
  annotated bibliography saying what each source decided. Purely internal
  cross-references get no `sources` entry, so most ADRs correctly have none.

* **Made the agent entrypoint vendor-neutral.** `AGENTS.md` is now the canonical
  file every agent reads; `CLAUDE.md` is a one-line `@AGENTS.md` import, since
  Claude Code auto-loads that filename. A pointer, not a fork — duplicating the
  rules into two files is the exact duplication those rules exist to prevent, so
  another agent means another thin pointer. Live cross-references were repointed;
  this log's earlier entries and ADRs 0008/0009 were left alone, being frozen
  records of what `CLAUDE.md` said at the time.

* **Resolved two contradictions the docs had carried.** `references/` was
  described as "immutable — read, never modify" while this repo genuinely
  ingests into it (ST 0603.5 landed 2026-07-27), which reads as a contradiction
  and makes an agent hesitate before a directed ingest; it is now stated as
  append-only, with immutability governing each snapshot rather than the
  directory's file count, and the case neither doc covered — a source believed
  wrong is never edited in place, it gets a `context/` concept citing it and
  saying where it departs. Separately, CONVENTIONS claimed broken links "are not
  errors", which is OKF *consumer*-tolerance semantics pasted into this repo's
  own policy: consumers must tolerate a dangling link, but this bundle must not
  ship one, and the link-check CI fails on it.

* **Fixed a blind spot that had been silently disabling the link check.** The
  workflow stripped HTML comments *before* code, so the bare `<!--` inside a
  documented code block paired with a `-->` far below and swallowed everything
  between — every check skipped that span, invisibly. Code spans are now removed
  first, comments last. Verified against a constructed case: under the old
  ordering a broken link below such a fence produced no hit at all; under the
  new one it is caught. The workflow also gained a dangling-footnote check
  (`[^label]` matching no `sources[].id` in the same file), and citations are now
  anchored by section rather than position — a line or page number into a `.txt`
  extract silently comes to point at the wrong text when the extract is
  regenerated, which is worse than a broken link because it still resolves and
  still looks right.

* Backported from `okf-project-template` (commits `6301ae4`, `5643ecb`,
  `a1e0a1c`, `46e4258`, `cabc297`), applied in dependency order rather than
  template order so each file was written once into its final home.

## 2026-07-31

* **Split the GStreamer backend by responsibility without changing behavior.**
  The thin backend facade now delegates extraction, insertion, and video/SEI
  work to private source units. Because the split changes ownership and callback
  seams rather than contracts, regression verification remains sensitive across
  extraction, insertion, live streaming, video passthrough, and GStreamer
  versions.

* **Made high-level streaming failures observable.** `KlvStream` drains queued
  valid Messages before exposing a terminal backend failure, makes a Message
  parse failure terminal, and reports normal EOS/cancellation as success.
  `KlvSink` preserves its exact opening error through `emit()` and `close()`.

* **Fork 25 — high-level streaming errors accepted:** terminal range status and
  sink-error propagation in
  [ADR 0027](./decisions/0027-high-level-streaming-errors.md).

* **Bounded incremental GStreamer KLV reassembly.** Extraction now distinguishes
  incomplete input from malformed BER, refuses declared frames above its
  configurable cap, resynchronizes only before a valid UL, and reports natural
  termination with a partial frame as `Truncated`. Cooperative cancellation
  remains successful.

* **Fork 24 — bounded live KLV frame reassembly accepted:** configurable 16 MiB
  complete-frame cap and framing outcomes in
  [ADR 0026](./decisions/0026-bounded-live-klv-reassembly.md).

* **Corrected the Message source/edit contract.** `has()` now reflects source
  items and staged additions; `encode()` uses private source membership so an
  unedited parsed Message returns only its original packet extent, preserving
  noncanonical BER and checksum placement without trailing input. `create()`
  continues to build packets from staged items.

* **Hardened VTarget Series, BER-OID/tag parsing, and packet framing.** VTarget
  Series declared lengths now use overflow-safe handling; BER-OID rejects
  forbidden leading-zero encodings and integer overflow, and decoded tags
  outside the current `uint16_t` Item model are rejected rather than truncated
  or aliased. Unknown in-range tags remain parseable. `packet_frame_length`
  requires the SMPTE UL prefix, while GStreamer extraction discards non-UL bytes
  to resynchronize and preserves a partial prefix across fragments. This is
  framing hardening only; resource limits and extraction-error policy remain
  deferred. Adversarial tests were added for the hardened boundaries.

* **Hardened typed encode validation and its adversarial tests.** Wrong typed
  alternatives, invalid numeric widths, raw-integer representability, and
  LinearLDS domain failures now follow the established encode error policy;
  IMAPB retains its ST 1201 structural-special behavior.

## 2026-07-30

* **Normalized spelling to the new American-English rule** across ADRs 0020,
  0021, 0023, 0024, this log, `tools/gst-container/`, and three spots in
  `src/gst/gst_backend.cpp` — two comments and one `g_warning` string
  ("an unrecognised format" → "unrecognized"). Full suite green afterwards.
  **Deliberately untouched**: `OutOfRangeBehaviour` is jmisb's Java class name,
  so respelling it in [`prior-art-jmisb`](./prior-art-jmisb.md) or in this log
  would misquote the API it names. `references/` is exempt by the rule itself.

* **Backported the planning-hygiene hardening from `okf-project-template`.** New
  rule: "Now" is where the *work* is, not a feature inventory — if a sentence
  would still be true after a month of no work it is durable knowledge and
  belongs in a `context/` concept or an ADR. The rule is in `CLAUDE.md`, its
  reasoning in [`workflow-rationale.md`](./workflow-rationale.md), which now also
  marks a rule *observed* where a real incident produced it and asks future
  sessions to append their own. This repo's own "Now" does not yet comply —
  recorded under PROGRESS § Known gaps.
* **Emptied PROGRESS "Now" of its feature inventory** (~85 lines → ~20), per the
  rule adopted earlier today. Nothing was lost: every claim was already owned by
  a concept, an ADR, or [`docs/api.md`](../docs/api.md), and the two kinds that
  had no durable home got one — the build/test/CI facts are now `CLAUDE.md`
  § Build / test / run (a section this repo never had), and the 0601 registry's
  coverage (every §8 item but the deprecated Item 66; embedded Local Sets and
  DLP/FLP/VLP packs as named opaque `bytes`) is now
  [ADR 0012](./decisions/0012-registry-codegen.md) § Consequences, beside the
  TOML it describes. "Now" is where the work is; the pointers carry the rest.
* **The link check now also resolves `[[slug]]` wikilinks** against concept
  names. They are not the house style here (this bundle uses sibling-relative
  markdown throughout), but a stray one no longer slips through unverified.
* **Fixed the link-check CI's blind spot.** It silently remapped root-absolute
  links onto `context/` before checking existence, so the one form CONVENTIONS
  bans was the one form it repaired — which is how ~68 of them accumulated here
  under green CI before the 2026-07-27 lint. It now flags them instead.
* **Adopted an explicit prose-style rule**: American English everywhere prose
  appears, matching the American-spelled MISB item names we quote, with
  `references/` and vendored code exempt (`CLAUDE.md` § Prose style).

## 2026-07-28

* **Reverted the stream-order change below — it caused two regressions, one of
  them silent data loss.** `gst_backend.cpp` is back to its pre-`4ced331` form:
  KLV is `0:0`, video is `0:1`. Full reasoning, both failed approaches and the
  measurements are now in
  [ADR 0020 § Stream order](./decisions/0020-video-passthrough.md).
  - **Dropped the KLV stream entirely, intermittently.** Deferring the `appsrc`
    link gets the PMT order right but leaves the pad *set* racy: the wait returns
    when the demuxer pad is linked while preroll carries on, so a muxer reaching
    its first output first wrote a **video-only PMT**. A playable file, exit 0,
    no telemetry — worse than the KLV-only PMT the wait was originally added to
    prevent, because that one fails loudly. 4/60 under load downstream; 0/52 on
    the pre-change build.
  - **Destabilised ST 0604 SEI timing, separately.** `linear time: SEI emitted`,
    `forward jump: Discontinuity reported`, `round trip: re-emitted`, `MP4 path:
    same frame count` all began flaking — 6/25 on an *idle* box against 0/25
    before. Linking the KLV branch after the video branch prerolls changes when
    its segment is established, and SEI matches PTS within a tolerance.
  - **A block probe on the muxer's src pad fixed the first and not the second**
    (0/60 on the PMT race, still 6/25 overall), which is what settled the revert:
    the deferral itself is the problem, not just what the muxer emitted.
  - **Reserving the video pad up front — the approach that should work — does
    not.** `mpegtsmux` refuses the later link onto an activated request pad with
    `GST_PAD_LINK_NOFORMAT`: it is checked against the parser's *current* caps
    (`avc` from the demuxer) against the muxer's `byte-stream`, though the
    *template* caps intersect fine. Explicit `gst_element_link_pads_filtered`
    onto the named pad fails identically. Measured, not reasoned.
  - **`gst_video_insert_test` now pins the order** (`PMT: KLV announced first
    (known, ADR 0020)`), so the next attempt is a visible change rather than
    something that slides in beside a regression.
  - The user guide and PROGRESS now warn that `0:0` is the metadata stream, so
    a consumer meets the limitation in the docs rather than in their output.
  - **The lesson is about evidence, not `mpegtsmux`.** The original change was
    green on its first suite run. Both regressions were timing-dependent and one
    only surfaced in a downstream consumer. Pipeline-construction changes need
    repeated runs under load before they are believed.

* **Video passthrough insertion now orders video before KLV in the output
  PMT** (`gst_backend.cpp`): stream 0:0 is video, 0:1 is KLV, verified with
  `ffprobe`. `mpegtsmux` assigns PID/stream order by the order sink pads are
  *requested*, not by `gst_bin_add_many` order — the KLV `appsrc` was being
  linked to the muxer up front, before the video demuxer's pad-added signal
  had fired, so it always claimed `sink_0`. Linking `appsrc` to the muxer is
  now deferred until after the video-pad wait loop, so the video pad (if any)
  requests its muxer sink pad first. No behavior change when there is no
  video source. Suite green (25/25).

* **`tools/gst-container/run.sh` verified end to end.** The docker daemon came
  back after the earlier hang; ran `run.sh` unmodified with no cached state
  assumptions beyond the already-built image — 25/25 under gstreamer 1.24.2.
  README's "not yet verified" caveat removed.

* **Video passthrough no longer needs a decoder installed to avoid decoding.**
  Fork 23, [ADR 0025](./decisions/0025-explicit-demuxer-passthrough.md): the
  chain is built explicitly — `filesrc ! demuxer`, container sniffed up front —
  instead of by `parsebin`.
  - **What parsebin was doing.** It decides a stream is fully parsed by asking
    whether any *decoder* in the registry accepts its caps. It never instantiates
    one; it just needs one to exist. Every ordinary system has one, so the
    dependency never showed. Restrict the plugin set to what this library
    actually uses and passthrough fails with `no suitable plugins found` —
    "missing parser" for a stream `h264parse` had already parsed.
  - Measured while packaging a bundle for `parrot-to-klv`: adding one H.264
    decoder fixes it; adding an unrelated decoder (vorbis, opus) does not, so the
    caps must match. A bundle would therefore need `openh264` (H.264 patents),
    `libde265` and `faad2` (GPL) — shipped to sit in a registry and never run.
  - Not just a packaging problem: needing an H.264 decoder in order to *not*
    decode H.264 is a latent defect on any minimal image.
  - **The parser table had to grow**, and this is the part parsebin was quietly
    covering: it plugged a parser for every stream, so the muxer always got
    framed data. A bare demuxer does not. MPEG-1/2 in `gst_video_insert_test`
    caught it within minutes — failing at `finish()`, not at link time. The table
    now names every codec the muxer needs framed, not only those needing a format
    conversion.
  - Containers are an explicit table now (MP4/MOV, MPEG-TS, Matroska) and
    anything else is refused at open with the container named. That is a real
    reduction in reach, taken deliberately; ADR 0020's promise — never decode,
    codec-agnostic — is unchanged.
  - Suite green: 25/25. `parrot-to-klv` converts H.264, H.265 and MP4-with-audio
    against eight LGPL plugins with no decoder present, which is what the whole
    thing was for.

* **Added `tools/gst-container/`** — the docker repro that found the hang, kept
  as a dev tool: `run.sh` builds this repo against a distro-pinned gstreamer
  (`UBUNTU=24.04` → 1.24, `22.04` → 1.20) and runs the suite, so the version gap
  that hid two bugs from a 1.20 host is testable without waiting on CI. Its
  package list mirrors the CI workflow's, deliberately.

  **The image and the in-container build+test are proven** (25/25 under 1.24.2,
  twice — that is how the bugs below were found). **`run.sh` itself is not**: two
  wrapper bugs were fixed while writing it, then the host's docker daemon went
  unresponsive before a clean end-to-end run. Committed as a starting point with
  that stated in its README; confirm it before relying on it.

  Also captured there: the MTU trap. Docker's bridge defaults to 1500 and WSL2's
  interface is 1420, so every packet over the host MTU is silently dropped and
  `apt` crawls rather than fails — 24 minutes versus 73 seconds. `run.sh` builds
  with `--network=host` for that reason.

* **The CI hang, found: a circular preroll deadlock, plus a version-dependent
  miss.** Reproduced locally at last by building the CI environment in docker
  (`ubuntu:24.04`, gstreamer **1.24.2**; this box has 1.20.3), which turned an
  11-minute CI round trip into a 90-second one. Two distinct faults, both only
  visible on 1.24:

  1. **Dropped streams deadlocked the pipeline.** A demuxer pushes every stream
     from one thread; a sink in PAUSED prerolls one buffer and then blocks that
     thread until PLAYING. So the `fakesink` taking the source's own KLV stream
     blocked the video queued behind it, the muxer never prerolled, the pipeline
     never reached PLAYING, and the sink never unblocked. `async=false` does not
     help — blocking in PAUSED is what a sink is *supposed* to do. The fix is a
     `queue` per dropped branch, `leaky=downstream`, so a stream we discard can
     never apply backpressure to one we carry. `tsdemux` logged it plainly once
     asked: `sparse stream, pushing GAP event`, and that thread never spoke again.
  2. **`Generate` stopped replacing the source's ST 0604.** 1.22 added a parsed
     payload type for `user_data_unregistered`; before that it arrived as an
     *unhandled* payload, which is all our detection knew about. On 1.24 the
     source's 418 SEIs were therefore not recognized and survived alongside ours
     — the exact duplication [ADR 0024](./decisions/0024-sei-generation-opt-in.md)
     exists to prevent, failing silently. Now handled both ways, guarded by
     `GST_CHECK_VERSION`.

  Suite green on both: 25/25 under 1.20.3 and under 1.24.2.

* **A second video pad was counted but never linked.** `on_video_pad_added`
  incremented `ignored_video_pads` and returned, leaving that pad dangling.
  parsebin requires every pad it exposes to be linked, and the resulting
  "not-linked" error is exactly the one the `open_insert` wait loop swallows as
  transient (from the MP4 audio fix) — so the pipeline can stall with nothing
  reported anywhere. Non-video pads have been dropped to a `fakesink` since that
  same fix; this path was missed. Now it is dropped the same way.

  Also: `gst_video_insert_test` line-buffers stdout. The first CI run under the
  new guards timed out *without* showing the test's own progress output — ctest
  kills the process and block-buffered `printf` output dies in the buffer, which
  is precisely what was needed to identify the failing case.

* **`finish()` could wait forever; CI hung for six hours a run because of it.**
  `Inserter::finish()` drained the pipeline with `gst_bus_timed_pop_filtered(...,
  GST_CLOCK_TIME_NONE, ...)` — the only unbounded wait in the library. A video
  branch that never reaches EOS therefore blocked the caller with no escape.
  CI had been hanging on `gst_video_insert` since `f9070fb` (the MP4 pad-linking
  fix, which is what made a stall reachable): the last green run was `f818fd9`,
  and nine runs afterwards sat in progress, one of them cancelled after **5h58m**
  on that single test. Bounded at 5 minutes — generous, because the drain carries
  whatever video is left and a caller that pushed its KLV early leaves most of the
  file to remux; it is a stall guard, not a performance budget.

  A timeout also had to change what success means: the old code read
  `ok = !(msg && ERROR)`, so a *null* message counted as success — harmless when
  the wait was infinite, wrong the moment it can time out. Now only a clean EOS
  passes, so a stall takes the ADR 0022 cleanup path instead of leaving a
  half-written output.

  CI gained the guards that would have caught this in minutes rather than hours:
  `timeout-minutes` on all three jobs (there were none, so a hang ran to GitHub's
  6-hour cap) and `ctest --timeout 600`, which names the offending test instead of
  killing the job anonymously. Nine hung runs cancelled.

  Not directly reproduced: the test passes locally in ~1.5 s and only stalls in
  CI, so the timeout is a fix for the unbounded wait itself, on the strong
  circumstantial evidence above.

* **CONVENTIONS § Linking said "either X or X".** The 2026-07-27 root-absolute
  lint rewrote the bad example in the guidance itself, leaving a sentence
  offering sibling-relative *or* "absolute" with two byte-identical examples.
  Rewritten to state the one style the bundle uses (418 sibling-relative links),
  why root-absolute resolves nowhere, and that the handful of `[[slug]]`
  wikilinks are understood but not the house style. Found while fixing the same
  section in parrot-to-klv, whose copy had kept the original bad advice.

* **Lint (drift sweep after forks 21-22).** Six stale claims, all left by this
  session's own work: `CLAUDE.md` still said passthrough generates ST 0604
  (README had been updated for opt-in, CLAUDE.md had not); ADR 0023 still said
  "always enabled" in its Decision list and Consequences, and its wire-format
  note gave `video_source` alone as the precondition; `backend-scope`'s B5 knew
  nothing about SEI generation or the caps-driven parser choice; ROADMAP cited
  only 0023 for the generation side in two places. Superseded claims now point
  at [ADR 0024](./decisions/0024-sei-generation-opt-in.md) rather than reading as
  current. Links resolve across every tracked `.md` in both repos.

* **Time Status bits 6/5 derived; `Generate` refuses non-H.264** (fork 22
  follow-ons, [ADR 0024](./decisions/0024-sei-generation-opt-in.md)) — the two
  items ADR 0023 had left open, both closed because both turn on what `Generate`
  means.

  Bits 6/5 (Normal/Discontinuity, Forward/Reverse) had been asserted. They are
  now computed in `push()`: the KLV's absolute time and the media timeline
  measure the same real seconds, so a packet whose two deltas disagree by more
  than 50 ms did not increment linearly — which is what bit 6 reports. Status is
  stored per map entry and travels with the timestamp. Bit 7 stays Lock Unknown.
  Driven in test with controlled timestamps: linear → `0x9F`, forward jump →
  `0xDF`, backward jump → `0xFF`. The existing battery's flat `0x9F` assertion
  had to be relaxed to shape-only — the `dayflight.klv` fixture's item 2 jumps
  6–98 *seconds* between packets pushed 100 ms apart, so the derivation flags it,
  correctly.

  `Sei0604::Generate` on non-H.264 now fails `open_insert` with
  `Error::Unsupported` instead of quietly producing video with no timestamps.
  Learning the codec to make that check **exposed a real bug**: `h264parse` was
  being inserted for *every* video pad since the MP4 audio fix, so ADR 0020's
  codec-agnostic claim had been false and an H.265 or MPEG-2 source could not
  link at all. The parser is now chosen from the pad's caps
  (`h264parse` / `h265parse` / none). Tests build H.265 and MPEG-1/2 sources at
  run time from `videotestsrc` (skipped where the encoder is absent) and cover
  both passthrough and the refusal. ADR 0020 amended.

* **Fork 22 — ST 0604 SEI generation is opt-in** (accepted,
  [ADR 0024](./decisions/0024-sei-generation-opt-in.md)): `Sei0604::Preserve`
  (default) / `::Generate` on `InsertConfig` and `KlvSink`, plus a
  `KlvSink(InsertConfig)` overload. Preserve restores the ADR 0020 property that
  fork 21 had cost — the passthrough video ES is **byte-identical** again, and
  the test asserts that rather than the size comparison 0023 had to settle for.
  Generate replaces a source's own ST 0604 rather than adding to it, closing the
  double-SEI question 0023 left open, and takes the Picture Timing stripping with
  it so a caller who never asked keeps theirs. Under Generate an unmatched frame
  is still scanned, so the source's SEI is removed even where we have nothing to
  put back: half-replacing would vary provenance frame to frame with nothing in
  the stream to signal it. `gst_video_insert_test` runs the whole battery in both
  modes. Consumer (`parrot-to-klv`) updated to opt in. Full CTest suite green,
  and green under ASan+UBSan.

* **Time Status now says Lock Unknown (`0x9F`)** — fork 21 revision, decided
  and recorded in [ADR 0023](./decisions/0023-st0604-sei-passthrough.md). The
  `0x1F` we had been emitting was correctly *encoded* but claimed the source
  clock was locked to an absolute time reference, which we cannot know: the
  timestamp is relayed out of ST 0601 item 2, which carries no lock
  information. Same failure mode as the relative-PTS fallback removed earlier
  the same day — a well-formed field asserting more than the code can support.
  `gst_video_insert_test` now asserts `0x9F` on every SEI we generate (the
  source's own passed-through SEIs keep their own status), so it cannot regress
  silently. Bits 6/5 (Normal/Forward) stay asserted rather than derived.

* **Ingest — ST 0603.5 (MISP Time System and Timestamps)**
  (`references/ST0603.5.pdf` + `.txt`, new [`st0603`](./st0603.md)): added to
  settle the one byte of our ST 0604 SEI payload that nothing in the bundle
  could check — the Time Status. §7.4 Table 3 confirms **`0x1F` is encoded
  correctly** (bit 7 = 0 Locked, bit 6 = 0 Normal, bit 5 = 0 Forward, bits 4-0
  reserved `11111`), and shows the code comment calling bit 7 "GPS locked" was
  wrong: it is an internal clock's lock to an absolute reference, not GPS.
  Comment corrected. Raised a follow-on question rather than a fix: we assert
  Locked unconditionally while relaying an ST 0601 item-2 timestamp that carries
  no lock information, so `0x9F` (Lock Unknown) may be the truthful value —
  recorded as open on [ADR 0023](./decisions/0023-st0604-sei-passthrough.md),
  since it changes bytes downstream readers see.

  Also makes first-hand what the bundle asserted secondhand: epoch
  1970-01-01T00:00:00Z, Precision Time Stamp = uint64 µs since epoch derived
  from UTC **without leap seconds** (§8), `Nano = 1000 × Precision` (§7.3),
  Commercial Time Stamp = SMPTE ST 12-1 time code (§7.5). Cross-linked from
  [`st0601`](./st0601.md), [`st0903`](./st0903.md), [`st0604`](./st0604.md),
  [`st0107`](./st0107.md); `index.md` entry added.

* **ST 0604 SEI probe rewritten against `codecparsers`** (fork 21 unchanged;
  revision recorded in [ADR 0023](./decisions/0023-st0604-sei-passthrough.md),
  `src/gst/gst_backend.cpp`, `test/gst_video_insert_test.cpp`): review of the
  fork 21 implementation found it hand-rolling H.264 byte scanning beside the
  `gstreamer-codecparsers-1.0` dependency it had added and never called. Four
  real defects — pointers reused across an unmap/remap (which could append
  uninitialized heap to every frame), an unbounded SEI scan that mis-parsed the
  0xFF-continuation syntax, a relative-PTS fallback that emitted well-formed
  ~1970 timestamps on a lookup miss, and an endianness assumption — plus a leak
  on a failed write mapping. The decision itself did not change. Wire format
  re-verified against ST 0604.6 §7.1 Table 1 / §7.4 Table 2 and requirements
  ST 0604.4-10/-12. Full CTest suite green, and green under ASan+UBSan.

  `gst_video_insert_test` now decodes ST 0604 SEI back out of the muxed output
  and requires every timestamp to be one the KLV carried — the coverage gap ADR
  0023 had flagged as open. Writing it surfaced a new one: **`data/klv_metadata_test_sync.ts`
  carries 418 of its own `MISPmicrosectime` SEIs**, so on such sources the
  output now holds two Precision Time Stamps per matched access unit — the
  source's and ours. Recorded as an open question on ADR 0023; it needs a
  decision about what passthrough owes the source's own data.

## 2026-07-27

* **Fork 21 resolved — ST 0604 SEI generation on the video passthrough path**
  (accepted, [ADR 0023](./decisions/0023-st0604-sei-passthrough.md)):
  `src/gst/gst_backend.cpp` +~240 lines, new `gstreamer-codecparsers-1.0`
  dependency, `gst_video_insert_test` relaxed from byte-exact ES to size-only.
  Verified by hand against parrot-to-klv (699 frames) and the downstream
  consumer's SEI decoder;
  full CTest suite green (25/25). Scopes down the ST 0604 deferral in
  [`0009`](./decisions/0009-st0604-deferred.md) and makes the passthrough ES of
  [`0020`](./decisions/0020-video-passthrough.md) no longer byte-identical.

* **Fork 21 opened — ST 0604 SEI, framed as *preservation***
  ([ADR 0023](./decisions/0023-st0604-sei-passthrough.md), `status: proposed` at
  this point): opened from a `parrot-to-klv` finding that its downstream client
  extracts ST 0604 timestamps from the video ES and wasn't getting them.
  Superseded within the day by the accepted entry above, which corrects the
  framing: Parrot MP4s have no SEI to preserve (timestamps live in an `mett`
  metadata track), so the task is *generation*, not preservation.

* **H.264 SEI/SPS association mitigation** (`src/gst/gst_backend.cpp`): set
  `h264parse config-interval=-1` (SPS/PPS with every IDR) to quiet downstream
  "didn't get the associated sequence parameter set" warnings when remuxing
  Parrot sources, and noted the residue as a known limitation on ADRs 0009/0020.
  The `config-interval` setting survives; the limitation notes do **not** — ADR
  0023 strips the offending Picture Timing SEI outright, so both notes were
  removed on 2026-07-28. Retained here as the chronology of how 0023 was reached.

## 2026-07-26

* **Fix MP4 video source pipeline failures** (`src/gst/gst_backend.cpp`):
  `KlvSink` initialization with MP4 video sources (via ADR 0020's video
  passthrough) failed with "Internal data stream error: streaming stopped,
  reason not-linked (-1)" from qtdemux. Two root causes: (1) **parsebin requires
  all pads to be linked** — audio/subtitle/metadata pads were ignored (returned
  early), leaving them unlinked and causing qtdemux to error; now linked to
  `fakesink` to satisfy parsebin. (2) **H.264 format mismatch** — parsebin
  outputs H.264 in "avc" format (with codec_data), but mpegtsmux requires
  "byte-stream"; now insert an `h264parse` element dynamically in
  `on_video_pad_added` to convert the format. Also: ignore transient
  "not-linked" errors during PAUSED state while pads are still being linked;
  wait for `linked` flag instead of failing immediately. Found by
  `parrot-to-klv` consumer using real Parrot MP4s with audio tracks.
  `gst_video_insert_test` already covered MP4 sources (remuxed from TS) and now
  passes; full CTest suite green (25/25).

* **Lint (docs consistency + drift)**, after the two fixes above. Four real
  findings, not cosmetics: (1) **~68 broken links** — most `context/` concepts
  wrote sibling links root-absolute (`](/st0107.md)`), which resolves nowhere on
  GitHub; now relative, and every relative link in the repo is verified to
  resolve. (2) **`MockBackend` contradicted the interface it exists to model** —
  it could only ever deliver `kNoPts`, i.e. the defect ADR 0021 fixed; it now
  takes an optional per-packet `pts` vector (omitted = an untimed stream, as
  before), covered in `mock_backend_test`. (3) **`data-samples.md` gained the PES
  timestamp characterization** — which captures are timestamped and which is not
  — the evidence ADR 0021 rests on. (4) **ROADMAP's Phase 5 had no status** while
  its contents (hardening pass, sanitizer CI, conformance-by-worked-examples,
  packaging, user guide) had landed; marked as such, with what keeps it open.
  Also refreshed `index.md`'s sample-data line (still described two files of
  five) and README's facade bullet (timing now carries through an edit).

* **Read-path timestamps** (fork 19 →
  [ADR 0021](./decisions/0021-read-path-timestamps.md)): `KlvPacket::pts_ns` was
  a documented field that neither extractor ever set, so every consumer saw `-1`
  on every packet of every file. Both now report **nanoseconds from the start of
  the source** — the same timeline `push()` writes on. The gstreamer backend uses
  the demuxer's running time (its segment is program-wide, so running time is
  zero-based at the source's start and is what `mpegtsmux` consumes on the way
  back out); `extract_ts_klv` converts the 90 kHz PES PTS and subtracts the
  earliest PTS in the buffer, found by a header-only pre-pass — the *minimum*,
  not the first in file order, which with reordered video is a frame late.
  Packets don't line up with the units carrying them (one PES can hold several
  packets, one packet can span two), so both extractors mark timestamps against
  absolute offsets in the reassembled byte stream and attribute to each packet
  the mark at its first byte — shared logic in the private `src/pts_marks.hpp`
  rather than two subtly different versions. Found by `parrot-to-klv`: since ADR
  0020 the writer *requires* a real PTS when there is a video branch, so
  `KlvStream` → edit → `KlvSink` had stopped composing.
  **The old "PES PTS is unreliable" finding was Day Flight, not gstreamer**: that
  capture's KLV PES genuinely carry no PTS (`PTS_DTS_flags` clear, verified
  byte-wise) and still correctly report `kNoPts`; `falls.ts` timestamps all 1 953
  of its KLV PES, and `Cheyenne.ts` / `klv_metadata_test_sync.ts` timestamp every
  `0x15` PES — which only the gst-free reader can see at all (ADR 0016). Notes
  added to ADRs 0013/0016/0017/0018 and `backend-scope.md`, whose claims this
  supersedes.
  **Tests**: `gst_video_insert_test` now asserts both extractors read back the
  timestamps it pushed (its own PES parser stays the independent witness), plus a
  `KlvStream` → `KlvSink` round trip over a video source that checks the timing
  survives; `ts_extract_test` checks the real captures are timestamped
  all-or-nothing and non-decreasing. Full CTest suite green, sanitizer build
  green.

* **"No output file on failure" now spans the insert session** (fork 20 →
  [ADR 0022](./decisions/0022-no-output-on-failure.md)): the guarantee ADR 0020
  established for `open_insert` stopped at the end of that function, and the same
  zero-byte `.ts` leaked one step later — a source whose video track is declared
  but unparseable opens fine, accepts pushes, and fails at `finish()`. Reproduced
  with an MP4 whose `avc1` entry has no `avcC`: `h264parse` refuses the caps and
  the muxer fails only at EOS. `open_insert` now hands the `Inserter` the sink
  path **only when this call created it**, so "never delete a file we did not
  create" holds by construction; a failing `finish()` unlinks it, a successful one
  clears the path, and destruction without a successful `finish()` unlinks too
  (an abandoned session produced an unfinalized file, not output). Unchanged: a
  pre-existing file is never deleted, though opening a file sink still truncates
  it. `gst_video_insert_test` covers the abandoned session and the late failure,
  both re-checking the pre-existing-file guard. Lets `parrot-to-klv` drop the
  local workaround it had written for exactly this.

## 2026-07-25

* **Video passthrough — consumer review fixes** (`parrot-to-klv` reviewed the
  branch and filed four findings; [ADR 0020](./decisions/0020-video-passthrough.md)
  amended for the behavior change).
  **The defect**: a failed `open_insert` could leave a zero-byte `.ts`. The
  pre-flight `fopen` check only catches a *missing* source; a readable one with
  no video stream fails later, in `PAUSED`, by which point the file sink has
  created its file — and a stale empty `.ts` reads as output to anything scanning
  the directory. Failures past pipeline construction now unlink the sink file,
  guarded by a probe taken *before* the state change so only a file this call
  created is ever removed. Not covered: the truncation itself — opening a file
  sink truncates whatever is at the path, on the success path too — so the
  guarantee is "we delete only what we made", not "your old file survives".
  **The test gap**: the suite only ever fed `parsebin` an MPEG-TS, i.e.
  `tsdemux`. The consumer feeds MP4 — `qtdemux`, a different demuxer negotiating
  different caps into the muxer — so a pad/caps regression could break the
  consumer's only path with the suite green. Rather than commit a binary fixture
  or depend on an encoder, the test now remuxes its own TS source into an MP4
  (`tsdemux ! h264parse ! mp4mux`, skipped if `mp4mux` is missing) and runs the
  whole battery a second time against it, asserting the same 418-frame count as
  the TS run. Its ES is deliberately not byte-compared: `avc` → byte-stream
  re-inserts parameter sets (2 774 895 vs 2 774 857 bytes, same frames).
  **Polish**: a non-TS source now *skips* the three source-comparison checks with
  a note instead of failing them (they need a TS to read, so pointing the binary
  at an MP4 used to show red for no reason); and the bus ERROR consumed during
  the pad wait is logged with its text via `g_warning` before being dropped —
  it can't reach `finish()` afterwards, and every caller-visible failure here
  collapses to `Error::Unsupported`. Full suite green.

* **Video passthrough on the insert path** (fork 18 →
  [ADR 0020](./decisions/0020-video-passthrough.md)): `InsertConfig::video_source`
  (and a matching defaulted `KlvSink` argument) adds a `filesrc ! parsebin`
  branch to the existing `mpegtsmux`, so one `open_insert` writes a TS carrying
  both video and KLV. Empty keeps the old pipeline exactly, so existing callers
  and `gst_insert_test` are untouched. Driven by the `parrot-to-klv` consumer
  spec.
  **What the implementation had to get right.** (1) `open_insert` prerolls in
  `PAUSED` and *waits* for `parsebin`'s video pad before `PLAYING` — otherwise a
  caller pushing KLV immediately races the video branch and the muxer writes a
  KLV-only PMT. (2) The first `video/*` pad is linked; all other pads are
  dropped, including the source's own KLV track (the sample source
  `klv_metadata_test_sync.ts` has one, so the test proves the drop). (3) With a
  video branch, `push(pkt, kNoPts)` is rejected — the synthesized ~30 fps counter
  would drift silently against real frame timing. (4) The source is checked for
  readability before any element is made, so a failed open leaves no partial
  `.ts`. `realtime` + video is rejected as unexercised.
  **Codec-agnostic, verified**: the same code path carried H.264-in-TS,
  H.264-in-MP4 (`avc` → byte-stream inside `h264parse`) and H.265-in-MP4 — the
  explicit `qtdemux ! h264parse|h265parse` fallback the spec allowed was not
  needed. New `gst_video_insert_test` (25 CTest cases now, all green) reads the
  output back with its own small TS/PES parser: PMT = exactly video + `0x06`/KLVA,
  KLV byte-exact, **video elementary stream byte-identical** to the source's (418
  PES, same codec), pushed PTS intervals preserved and sharing the video's origin.
  Note for future readers: `mpegtsmux` starts the TS clock an hour in, so absolute
  90 kHz PTS are not the pushed nanoseconds — intervals and shared origin are the
  invariants.

* **ST 0601 registry breadth — the full item set**: the registry was a
  Milestone-1 subset (27 items, the ones Day Flight's first packet carries);
  everything else round-tripped as raw bytes. Transcribed the remaining
  §8.1–8.143 items from the standard's per-item format blocks, less Item 66
  (deprecated by 0601.19, no format defined — stays unregistered). Typed by the
  KLV column: `linear_lds` for the mapped scalars (signed items symmetric,
  unsigned ones carrying the offset in `min`, e.g. altitudes over -900..19000),
  `imapb` for the ST 1201 extended items, `uint`/`int`/`utf8` for the direct
  ones, and `bytes` for what is structurally opaque to 0601 — other standards'
  Local Sets (0102, 0806, 1002, 1010, 1204, 1206, 1601, 1602, 1607) and the
  DLP/FLP/VLP packs it defines item-locally. Those keep the exact raw
  round-trip an unregistered tag had; what they gain is a name. Also added the
  `0x8000` out-of-range specials that Items 6/7 had been missing.
  **Two mechanical points.** 0601's IMAPB and extended-integer items are
  *variable-length* (the sender picks the width), but a descriptor still needs
  an encode width, and `message.cpp` reaches for `fixed_len` when setting a tag
  on a fresh packet. So `length` on those items now means *default encode
  width* (chosen from the item's documented per-width resolution — e.g. 3 B for
  the ±0.078 m altitudes, 2 B for the angles), and a new explicit
  `variable = true` key keeps the descriptor honest about the standard allowing
  others; decode was already length-driven (`imapb_decode` reads `raw.size()`),
  so it honours whatever width arrives.
  **Validation** — three independent layers: (1) every `data/`-derived fixture
  now decodes with **zero unregistered tags** and stays **byte-exact** through
  the full-stream round-trip (cheyenne, falls, falls_ext, sync, dayflight,
  nightflight_ir), which exercises the newly typed 3/4/10/26–33/47/48/59/72/
  75/78/82–91 on real vendor data; (2) a new `st0601_examples_test` runs **the
  standard's own worked examples** (the "Example Software Value / Example KLV
  Item" pair ending each §8.N block) — one vector per distinct
  kind/range/width family, since a byte-exact round-trip only proves encode and
  decode are mutual inverses and a mis-scaled mapping satisfies that too;
  (3) clean under ASan+UBSan. Tolerance there is one quantization step: the
  spec prints its example values rounded, and tags 26 and 83 land a fraction of
  an LSB off the exact mapping. ADR 0012's invariants re-verified — regeneration
  is deterministic and the no-tomllib fallback reader still produces identical
  output (checked with both TOML libraries blocked).
* **Workflow backport from `okf-project-template`**: the doc method extracted
  from this repo into a reusable template gained refinements worth bringing back.
  Added [`workflow-rationale.md`](./workflow-rationale.md) — the *why* behind each
  planning-hygiene rule (one job per doc, same-commit docs, no live tallies,
  closing scrubs the future, lint-as-backstop), so a rule can't be dropped for
  being inconvenient without meeting the failure mode it prevents; indexed it and
  pointed `CLAUDE.md` at it. Added a **Before-every-commit checklist** to
  `CLAUDE.md` (code staged → `log.md` → `PROGRESS.md` → ADR + register) with the
  same-commit rule made explicit on the implementation bullet. Defined **fork** =
  a decision point (not a git fork) canonically in `CONVENTIONS.md` § Decisions,
  referenced from `CLAUDE.md` + ROADMAP. Fixes: the ADR-format § Register line
  described the old `Title | Status` shape (register is `Fork | ADR | Status`);
  the decision lifecycle omitted `deferred`; the lint section had no cadence; and
  § Linking mandated `/`-absolute links while the bundle overwhelmingly uses
  `./` (now: either, resolved from `context/` — matching what CI checks).
* **Lint (against the newly-stated rules)**: audited the existing docs for what
  the backported rules would catch. Clean: every ADR is in the register with a
  `fork:` and a terminal status (none stuck at `proposed`), no orphan concepts,
  no live tallies in PROGRESS/ROADMAP, all internal links resolve. Two fixes —
  ADR [`0018`](./decisions/0018-high-level-api.md) called generated tag enums "a
  possible follow-on" after that follow-on had landed (added a *Since accepted*
  note rather than rewriting the rationale — an ADR records its moment), and
  `type: Conventions` was in use by two files but missing from the type
  vocabulary. Deliberately **not** retrofitted: older `log.md` entries and ADR
  bodies keep their frozen counts and phrasing — history is a record, not a
  present-tense claim, and the no-tallies rule was never about it.
* **CI — internal Markdown link check** (`.github/workflows/link-check.yml`, from
  the template's dormant workflow, enabled): fails on an internal link whose
  target doesn't exist, ignoring inline-code and HTML-comment examples and
  resolving `/`-absolute links from `context/` per the linking convention. All
  links currently resolve, so it lands green. This is the only automated part of
  the CONVENTIONS lint; the rest stays a periodic read-through. Also normalized
  line endings (`* text=auto` in `.gitattributes`).

## 2026-07-19

* **Scope (gstreamer backend)**: probed the environment + extraction path to
  ground ADR [`0008`](./decisions/0008-media-backend-gstreamer.md); wrote
  [`./backend-scope.md`](./backend-scope.md). gst 1.20.3, all
  elements present, **`gstreamer-mpegts-1.0` dev missing** (need
  `libgstreamer-plugins-bad1.0-dev` for the PMT rewrite). **Key finding** (via
  python-gi `tsdemux` pad probe across all samples): stock `tsdemux` exposes
  `0x06`+KLVA as `meta/x-klv` (Day/Night/`falls`) but **silently drops `0x15`
  metadata** (`Cheyenne`, `klv_metadata_test_sync`) — extraction is two regimes,
  and ADR 0008's "accept both on extract" is a real requirement. Opened ROADMAP
  forks 11–14 (interface / 0x15 extraction / `klvpmtrewrite` / optional-dep build).
  Resolved the `0x15` signaling caveat in [`data-samples`](./data-samples.md).
* **B0 extraction spike (backend)**: installed `gstreamer-mpegts-1.0` dev; ran
  `tsdemux ! meta/x-klv ! appsink` on Day Flight via python-gi. Extraction is
  **byte-identical** to the ffmpeg-extracted `.klv` the core already round-trips —
  the core↔gst path is proven. Two interface inputs (recorded in
  [`backend-scope`](./backend-scope.md)): appsink yields sub-packet
  fragments (203 buffers / 6 packets → backend reassembles + `parse_packet`), and
  PES PTS is unreliable (`CLOCK_TIME_NONE`) so correlation should use KLV Item 2.
* **Decision (proposed)**: Fork 11 →
  [`0013-media-backend-interface`](./decisions/0013-media-backend-interface.md)
  (proposed). Abstract `MediaBackend`: blocking `extract(source, on_packet)`
  yielding framed per-packet `KlvPacket{bytes, pts_ns}` (bytes borrow the
  backend's reassembly buffer — the ADR 0011 boundary; consumer runs
  `parse_packet`) + `open_insert(config) → Inserter{push, finish}` with appsrc
  backpressure. Byte-level (no decode); `Result<T>`; `GstBackend` + `MockBackend`;
  interface header core-only (gstreamer confined to `GstBackend`, fork 14).
  Rejected: pull-iterator, parsed-`Packet` units, raw-buffer units,
  need-data-callback insertion, no-interface. Grounded by the B0 spike. ROADMAP
  fork 11 → PROPOSED.
* **Decision (accepted)**: [`0013`](./decisions/0013-media-backend-interface.md)
  → accepted. `MediaBackend` interface locked; B1 (GstBackend extraction +
  MockBackend + optional-dep CMake) implements it. ROADMAP fork 11 → DECIDED.
* **Decision (accepted)**: Fork 14 →
  [`0014-backend-optional-dependency`](./decisions/0014-backend-optional-dependency.md)
  (accepted): a **separate `misbklv-gst` CMake target** holds the gstreamer impl;
  the core `misbklv` stays dependency-free. `option(MISBKLV_GSTREAMER)` +
  `pkg_check_modules`; skipped if gstreamer absent. Interface/factory headers are
  gstreamer-free. ROADMAP fork 14 → DECIDED.
* **B1 (implementation)**: media backend — extraction + interface + mock.
  `include/misbklv/backend.hpp` (ADR 0013 interface: `MediaBackend`, `KlvPacket`,
  `Inserter`), `mock_backend.hpp` (`MockBackend`/`MockInserter`, core-only),
  `gst_backend.hpp` factory + `src/gst/gst_backend.cpp` (real gstreamer:
  `filesrc ! tsdemux ! appsink`, reassemble appsink fragments, frame packets via
  new `packet_frame_length`). Added `Error::{Backend,Unsupported}`. Tests:
  `mock_backend` (contract) + **`gst_extract`** — GstBackend extracts
  `Day Flight.mpg` into **6 whole packets, byte-exact vs the committed `.klv`**
  (each `parse_packet`-able). 14 CTest cases; CI installs gstreamer. Insertion is
  B2 (`open_insert` returns `Unsupported`).
* **Decision (accepted)**: Fork 13 →
  [`0015-no-pmt-rewrite`](./decisions/0015-no-pmt-rewrite.md) (accepted).
  **`klvpmtrewrite` is NOT needed.** B2 spike (gst 1.20.3): stock
  `appsrc(meta/x-klv) ! mpegtsmux` already emits `stream_type 0x06` + `KLVA`
  registration descriptor; insert→re-extract is byte-exact. Supersedes ADR 0008's
  PMT-rewrite requirement (its premise was true only for older gstreamer). ADR
  0008 annotated; ROADMAP fork 13 → DECIDED.
* **B2 (implementation)**: media backend insertion. `GstInserter` in
  `gst_backend.cpp` = `appsrc(meta/x-klv) ! mpegtsmux ! {file,udp,srt} sink`;
  `push()` (blocks on backpressure) + `finish()` (EOS/drain); `make_sink()` parses
  `file:`/`udp:`/`srt:`. `open_insert` builds+starts the pipeline. `gst_insert`
  test: KLV → mux → `.ts` → re-extract **byte-exact** (15 CTest cases). Both
  directions now work with **stock gstreamer, no custom element** — B2 collapsed
  from "build klvpmtrewrite" to a thin inserter.
* **Decision (accepted)**: Fork 12 →
  [`0016-ts-0x15-extraction`](./decisions/0016-ts-0x15-extraction.md) (accepted):
  a gst-free MPEG-TS KLV extractor for the `0x15` streams stock `tsdemux` drops.
* **B3 (implementation)**: `0x15` extraction. PES probe showed `0x15` wraps KLV in
  SMPTE RP 217 metadata AU cells (`stream_id 0xfc`, 5-byte cell header) vs `0x06`
  (KLV directly in PES). New core `extract_ts_klv` (`ts.hpp`/`src/ts.cpp`, in the
  `misbklv` lib, no gstreamer): iterates 188-byte TS packets, finds the KLV PID by
  content (PES payload starting with a SMPTE UL), reassembles PES, unwraps the AU
  cell for `0x15`, frames via `packet_frame_length`. `ts_extract_test`: **byte-exact
  vs ffmpeg** on Day Flight (0x06), Cheyenne + sync (0x15); each unit
  `parse_packet`-able. 18 CTest cases. **Bonus: file KLV extraction is now
  gstreamer-free** — gstreamer only needed for live sources + mux.
* **Decision (accepted)**: Fork 15 →
  [`0017-realtime-streaming`](./decisions/0017-realtime-streaming.md) (accepted):
  live-sink clock pacing + live-source idle-timeout termination. Rejected a
  handler stop-token (changes the ADR 0013 signature), over-the-wire EOS (udp has
  none), always-on `sync`, and RTP payloading (deferred).
* **B4 (implementation)**: real-time streaming, closing the ADR 0008 goal.
  `open_insert` honors `InsertConfig::realtime` (`appsrc is-live` + sink `sync` →
  clock-paced, `push` blocks at stream rate); `extract` gained `make_src` so a
  `udp:`/`srt:` source uses `udpsrc`/`srtsrc` and ends on a `udpsrc` idle timeout
  (500 ms, ignored until ≥1 packet flows so startup can't truncate) since no EOS
  crosses the wire. `gst_stream_test`: one-process **udp loopback** (receiver
  `extract()` on a thread, realtime `push` on main) → **byte-exact**, 6 packets in
  ~165 ms (5×33 ms = genuinely clock-paced), stable across repeated runs.
  19 CTest cases. **Backend B0–B4 complete**; extraction + insertion, file + live,
  all on stock gstreamer with no custom element.
* **Restructure (planning/context seams)**: de-duplicated the ripple-update
  workflow. [`decisions/index.md`](./decisions/index.md) gained a Fork column and
  is now the sole **decided-fork register**; [`ROADMAP`](../planning/ROADMAP.md)
  dropped its duplicate fork table (keeps scope / phases / *open* forks, defers to
  the register). [`PROGRESS`](../planning/PROGRESS.md) is now present-tense only —
  its ~200-line "Done" history graduated here (this log owns chronology).
  [`backend-scope.md`](./backend-scope.md) moved `planning/` → `context/` as a
  `type: Component` concept (durable design analysis, not plan/state). Root
  `CLAUDE.md` + [`CONVENTIONS`](./CONVENTIONS.md) updated to codify the
  one-job-per-doc split.
* **Conventions (doc hygiene)**: two ripple-workflow rules, no tooling. (1)
  Decision log entries are a **single thin line** (chronology + link; the ADR owns
  the rationale — [`CONVENTIONS`](./CONVENTIONS.md) § Decisions), a second entry
  only on a cross-session gap or revision. (2) **No live tallies** (test-case
  counts, item totals) in present-tense / durable prose — a specific number
  belongs only in a dated log snapshot here (root `CLAUDE.md` Planning hygiene).
* **Hardening (implementation)**: real-world robustness pass — inputs the clean
  vendor samples never exercised. Fixed **integer-overflow → OOB** in the length
  arithmetic (`parse_packet`, `parse_items`, `packet_frame_length`: a crafted
  8-byte BER length wrapped past a naive bound check into an out-of-range
  `subspan`), and added **length validation to `codec::decode`** (numeric kinds
  reject 0- / >8-byte values — a 0-length item drove shift-by-negative UB; also
  made the signed-linear `imax` shift unsigned to avoid `1ll<<63`). Confirmed
  **multi-byte BER-OID tags (≥128)** already parse/build correctly (test gap, not
  a bug — `read_oid`/`write_oid` handle base-128) and **Report-on-Change trimmed**
  packets round-trip. New `hardening_test` covers all three; the core builds clean
  under **ASan+UBSan** (`MISBKLV_SANITIZE` option + a CI sanitizer job, gstreamer
  off). Verified the test has teeth: reintroducing the overflow flips it to FAIL.
* **Decision + API (fork 16)**: high-level facade —
  [`0018`](./decisions/0018-high-level-api.md) (accepted). `Message` (core): an
  owned, editable packet — copy+parse, registry auto-selected by UL key, typed
  `get<T>`/`set`, `encode()` byte-exact for untouched items (move-only; owns above
  the borrow core). `KlvStream`/`KlvSink` (`misbklv-gst`): range-for read (backend
  push-extract adapted to pull via a background thread + bounded queue), edit,
  emit — file + live. `message_test` + end-to-end `api_test` (read Day Flight,
  edit SensorLatitude, emit, re-read — all 6 frames' edits persist through
  decode→encode→mux→demux), a `klv_edit` example, and the first user guide
  [`docs/api.md`](../docs/api.md). Closes the Phase-3 usability pass.
* **Decision + cancellation (fork 17)**: cooperative extract stop —
  [`0019`](./decisions/0019-extract-cancellation.md) (accepted). `extract` gained
  a `std::stop_token` (default = never signaled, so existing callers are
  unchanged); `GstBackend` polls it on a 100 ms bus timeout and tears down on
  request, `MockBackend` checks it between packets. `KlvStream`'s destructor now
  `request_stop()`s (after unblocking the queue so `set_state(NULL)` can't
  deadlock), so breaking out of the read loop over an **endless** live source
  returns promptly instead of hanging — retiring the 0018 caveat. `stop_test`
  (endless mock): early break destroys in ~6 ms; verified teeth — removing the
  cancel makes it hang.
* **Packaging fix (gst was not consumable)**: an out-of-tree build check found
  that `find_package(misbklv)` installed only the core — `libmisbklv-gst.a` and
  its target were never exported, so the advertised `misbklv::gst` (KlvStream/
  KlvSink) couldn't be linked. Fixed: gst is now an opt-in **find_package
  component** (`find_package(misbklv COMPONENTS gst)` → `misbklv::gst`), exported
  with `EXPORT_NAME gst` (was leaking as `misbklv::misbklv-gst`), its Config
  re-discovers gstreamer + Threads so the imported-target link resolves; core-only
  consumers keep zero gst dependency. Verified by building both a core and a gst
  consumer against a fresh install (gst consumer runs KlvStream over Day Flight).
  CI's install step now **builds a consumer** (both components) — the bare
  `cmake --install` smoke test had masked this. Completes ADR 0014's intent.
* **Pushed to GitHub + first-CI fix**: `git@github.com:nitsuga/libmisbklv` (main,
  ~431 MB LFS). The first Actions run failed one test — `stream_falls` — in both
  the build and sanitizer jobs: the regression extracted KLV from `data/*.ts` with
  **ffmpeg at build time**, and ffmpeg 6 (ubuntu-latest) frames `falls.ts` stream 0
  differently than local 4.4 (385462 vs 395227 B → first packet no longer on a
  boundary → Truncated). A demuxer's exact byte output isn't a stable contract, so
  **committed the four extracted streams as fixtures** (`test/fixtures/{cheyenne,
  falls,falls_ext,sync}.klv`, ~1.14 MB) and dropped build-time extraction; the
  `stream_*` tests read them directly and `ts_extract_*` cross-checks our
  `extract_ts_klv` against them (verified byte-exact vs the committed refs).
  **ffmpeg removed from the tests and both CI jobs** — the regression is now
  hermetic (LFS `.ts`/`.mpg` still needed only for `ts_extract`).
* **Repo basics + README**: set the GitHub description + topics; freshened
  `README.md` (CI badge, features, read/edit/write quick-start, `find_package`
  core-vs-`COMPONENTS gst`, and a build-deps section — GStreamer split into
  build-time dev files vs runtime plugins, per-distro). Branch protection skipped
  (unavailable on private + free plan; repo stays private).
* **Lint (docs current)**: reconciled the living docs with the resolved state.
  `backend-scope.md`'s "Open decisions" (forks F-A–F-D, all resolved by ADRs
  0013/0016/0015/0014) → "Design forks — resolved"; retired its `klvpmtrewrite` /
  `gstreamer-mpegts` / "First step" leftovers (superseded by ADR 0015) and added
  a status banner + Outcome. ROADMAP: "forks 1–15" → "1–17"; dropped the resolved
  candidates (multi-byte BER-OID tags — done in the hardening pass; the live-extract
  stop-token — ADR 0019). PROGRESS "Next": stop-token removed. All ADRs are
  accepted/deferred (none stuck at proposed).
* **Process amendment (why that drifted)**: root cause — *forward-looking*
  content (open-decision / candidate / "Next" lists, a fork-count range) sits off
  the decision update-path, and was duplicated across ROADMAP + PROGRESS + ADR
  deferrals — the future-tense analog of the history duplication the earlier
  consolidation removed. Codified the fix in `CLAUDE.md` Planning hygiene:
  (1) extended "no live tallies" to **fork ranges** ("none open, see the register",
  not "1–N"); (2) added **"closing scrubs the future"** — resolving an item deletes
  its forward-looking mentions, not just the present-state bullet; (3) **one home
  for open work** — ROADMAP owns the candidate-*fork* backlog, PROGRESS "Next" is
  immediate *work* + a pointer (collapsed the live ROADMAP/PROGRESS overlap).
  Added forward-looking staleness to the CONVENTIONS lint checklist (the backstop).
* **Finding + candidate (live 0x15)**: confirmed 0x15 metadata is **offline-only**
  — the live gst path (`…src ! tsdemux ! appsink`) drops 0x15 silently, and the
  0x15 handler `extract_ts_klv` is a whole-buffer batch parser, not streaming.
  0x06 works live + offline. Opened a ROADMAP candidate: a streaming incremental
  TS demux fed from a raw `udpsrc`/`srtsrc ! appsink` (bypassing `tsdemux`),
  unifying 0x06 + 0x15 on one gst-free live path; extends
  [`0016`](./decisions/0016-ts-0x15-extraction.md).
* **`Message::create` (from-scratch authoring)**: the high-level facade was
  parse-only; authoring meant dropping to `LocalSetBuilder` + the low-level
  `Inserter`. Added `Message::create(RegistryId)` → an empty packet for a
  standalone type (0601 / VMTI; errors for a pack like Vtarget), populated with
  the same `set()`, emitted via `encode()` / `KlvSink::emit` — completes ADR
  0018's write side symmetrically with read. `message_test`: create → set →
  encode → re-parse round-trips typed values, and byte-exact re-encode confirms
  the checksum. Documented in [`docs/api.md`](../docs/api.md) (nested sets /
  series / mandatory enforcement still point to `LocalSetBuilder`).
* **Named tags (ADR 0018 follow-on)**: `get`/`set` took raw tag numbers only.
  `gen_registry.py` now also emits a per-registry `enum class` from the item
  names (`namespace misbklv::tags`: `Uas0601::SensorLatitude = 13`, acronyms
  preserved, member-name collisions are a generator error). Added `get`/`set`
  overloads accepting any enum whose underlying type is `uint16_t`, forwarding to
  the number path — so `msg.get<double>(tags::Uas0601::SensorLatitude)` and the
  number are interchangeable; numbers still work for unregistered tags.
  Regenerated the committed tables (deterministic + fallback-reader parity, so the
  ADR 0012 drift check passes); `message_test` uses names; `docs/api.md` updated.

## 2026-07-18

* **Ingest (Prior Art)**: Added [`jmisb`](./prior-art-jmisb.md) (WestRidgeSystems,
  Java, **MIT**) — the most complete peer found (0601/0903/1201/0102 + ~20 more).
  Generic KLV core (Ber*/KlvParser/LdsParser/UdsParser/UniversalLabel/ArrayBuilder)
  + **class per item** (the [`0006`](./decisions/0006-tag-registry.md)-rejected
  approach — a foil, not a template). Value: **JUnit byte-array test vectors** as
  ground truth for our hand-authored fixtures, and `st1201/FpEncoder` +
  `OutOfRangeBehaviour` to cross-check our IMAP (Zoffset edge + unimplemented IMAP
  special-value semantics). Flags concepts we deferred: UDS (vs LDS) and Array type
  (0903 §9.1.2). MIT → reference freely, attribute if lifting a vector verbatim.
  Index Prior Art section updated. Cross-check of vectors: next.
* **Cross-check (IMAP)**: ran our `imapb` codec against ST 1201 Annex A vectors
  transcribed by [`jmisb`](./prior-art-jmisb.md) (`FpEncoderThreeByteTest`,
  `ST1201AnnexATest`). Added 8 vectors to `imapb_test` across 4 ranges —
  `IMAPB(0,100,3)`, **`IMAPB(-9.9,110,3)` (non-zero Zoffset)**, `IMAPB(0.1,0.9,2)`
  — **all pass** (e.g. `IMAPB(-9.9,110,3)` enc(0.0)=0x09E667, enc(110)=0x77E667).
  Independent confirmation that our IMAP forward mapping + the M6 Zoffset fix are
  correct. **Gap found**: the vectors include IMAP *structural* special values
  (NaN=0xD0.., ±∞=0xC8/0xE8.., below/above = 0xE0/0xE1.., top-2-MSB signaling)
  which our codec does not yet handle (only explicit per-item patterns) — logged
  to PROGRESS Known gaps; jmisb's `OutOfRangeBehaviour` is the reference.
* **Cross-check (0601/0903)**: new `jmisb_crosscheck_test` (8th CTest case)
  validates our registered codecs against [`jmisb`](./prior-art-jmisb.md) item
  test vectors (MIT). **0601 linear-LDS** (not covered by the IMAPB-only ST 1201
  vectors): SensorLatitude (int32 ±90: −90=0x80000001, 90=0x7FFFFFFF,
  60.1768229669783=0x5595B66D), SensorTrueAltitude (uint16 −900..19000:
  14190.72=0xC221), PlatformHeadingAngle (uint16 0..360: 159.9744=0x71C2) — all
  byte-exact, **independently confirming 0601 lat/lon/angles are legacy linear,
  not IMAP** (the M-era KB correction to [`st0601`](./st0601.md)). **0903 VTarget**:
  targetLocationOffsetLat = IMAPB(−19.2,19.2,3) (10.0°=0x3A6667, confirms our
  registry range matches jmisb) and targetColor uint24 (RGB 85,136,51=0x558833).
  jmisb's 0601/0903 messages are built dynamically (no single golden packet), so
  this cross-checks per-item codecs; full-packet vectors remain future work.
* **Research (0604 / post-v1)**: scanned [`jmisb`](./prior-art-jmisb.md) for ST
  0604 — **none** (no SEI/NAL/H.26x in the whole repo; KLV-only). Confirms 0604
  is a video-ES subsystem, not KLV. Reusable seam = **ST 0603 time** (jmisb
  `st0603`, = our 0601 item-2 semantics); only the ES-embedding is new. Recorded
  on [`0009`](./decisions/0009-st0604-deferred.md) (revisit notes) + the jmisb
  prior-art doc.
* **Characterize (Sample Data)**: extracted + walked the three new `data/*.ts`
  ([`data-samples`](./data-samples.md)). **All ST 0601, no VMTI** (tag 74 absent
  everywhere — 0903 read path stays on hand-authored + jmisb vectors). Cheyenne
  (407 pkts), falls (**two** KLV streams: basic ≤tag65 + extended ≤tag91, 1953
  each), sync (365). **All four streams round-trip byte-exact = 4678 real packets**
  through parse→codec→builder→checksum — strongest validation to date. Surfaced:
  registry-breadth items (3/4/10 strings, 26–33 corner offsets, falls-2's 90+
  IMAPB extended items); **tag 48 = Security LS (ST 0102)** nested instance;
  multi-byte BER-OID tags (≥128) still unexercised (max tag 91). Not yet wired as
  CTest (needs committed `.klv` fixtures or build-time ffmpeg extraction).
* **Regression tests (build-time extraction)**: wired the `data/*.ts` as CTest
  cases (`stream_cheyenne`/`falls`/`falls_ext`/`sync`). CMake `find_program(ffmpeg)`
  + `add_custom_command` extracts each KLV ES to `build/*.klv` at build time (ALL
  target `extract_ts_klv`); no committed `.klv` bloat. Skipped if ffmpeg absent;
  CI must `git lfs pull` the `.ts` first. 12 CTest cases total, all pass.
* **IMAP special values (implementation)**: closed the gap the jmisb cross-check
  found. `codec.hpp` now handles ST 1201 §7.2.3 structural specials (top-2-bits
  set): `imapb_special_of` classifies by the Table 2/3 patterns; `imapb_encode`
  signals NaN→`0xD0`, +∞→`0xC8`, −∞→`0xE8`, x<min→`0xE0` (BELOW), x>max→`0xE1`
  (ABOVE); `imapb_decode` returns IEEE values or clamps below/above→min/max (ST
  1201 default reverse). Public `is_imap_special()` for reserialization
  raw-passthrough (below/above decode lossily, so don't round-trip through the
  codec alone). `imapb_test` gained the jmisb `IMAPB(0,100,3)` special vectors —
  all pass. No regressions (12/12).
* **Library split (plumbing)**: converted the header-only core into a compiled
  static library `libmisbklv.a`. Moved function/method bodies from `ber`/`codec`/
  `packet`/`builder`/`series`/`registries` headers into `src/*.cpp`; public
  headers are now declaration-only. `types.hpp` (types + `Result<T>` template +
  `Registry::find`) and the generated `constexpr` tables stay header (must, for
  templates/compile-time). Internal codec helpers moved to an anonymous namespace
  in `codec.cpp`; public codec API narrowed to what's used cross-TU
  (`rd_uint`/`decode`/`encode`/`imapb_*`/`bcc16`/`is_imap_special`). Added
  umbrella `misbklv.hpp`. CMake: `INTERFACE` → `STATIC` target. All 12 tests link
  the lib and pass. Remaining plumbing: install/export + CI config.
* **Install/export + CI (plumbing)**: made the library installable and consumable.
  Moved the generated tables `src/registry/` → `include/misbklv/registry/` (clean
  single-`include/` install tree; dropped `src` from public include dirs, updated
  includes + the regenerate target, which also gains the previously-missing
  vtarget0903). CMake `install(TARGETS EXPORT)` + `configure_package_config_file`
  → `find_package(misbklv)` gives `misbklv::misbklv`; **verified end-to-end** by
  installing to a prefix and building an out-of-tree consumer. (Bug caught + fixed:
  `include(GNUInstallDirs)` must precede the `INSTALL_INTERFACE` include-dir GE, or
  it's empty.) Added `.github/workflows/ci.yml`: build+test (LFS + ffmpeg), install
  smoke test, and the ADR 0012 drift check job. ADR 0012 path refs updated.

## 2026-07-17

* **Initialization**: Established OKF v0.1 bundle — `index.md`, `log.md`, `CONVENTIONS.md`.
* **Schema**: Seeded open `type` vocabulary (Standard Reference, Encoding Rule, KLV Item, Prior Art, Decision, Component) in `CONVENTIONS.md`.
* **Pointer**: Filled root `CLAUDE.md` as the auto-load entry into this bundle.
* **Ingest**: Added five `type: Standard Reference` concepts —
  [`st0107`](./st0107.md) (KLV core), [`st1201`](./st1201.md) (IMAP mapping),
  [`st0601`](./st0601.md) (UAS Datalink LS), [`st0903`](./st0903.md) (VMTI),
  [`st0604`](./st0604.md) (ES timestamps). Cross-linked dependencies;
  populated `index.md` Standard References section.
* **Ingest**: Added five `type: Prior Art` concepts —
  [`klvdata`](./prior-art-klvdata.md), [`klvp`](./prior-art-klvp.md),
  [`libmisb0601`](./prior-art-libmisb0601.md),
  [`gstklvplugin`](./prior-art-gstklvplugin.md),
  [`akrutsinger/libklv`](./prior-art-libklv-akrutsinger.md).
  Key takeaways: parser-vs-item-DB split (klvp), INI-driven tag registry +
  PMT `0x06`+`KLVA` signaling + element decomposition (gstklvplugin), the
  ffmpeg `-map data-re` demux idiom on our exact sample (klvdata). Flagged
  the `akrutsinger/libklv` name collision for a future Decision.
  Populated `index.md` Prior Art section.
* **Ingest**: Added [`data-samples`](./data-samples.md) (`type: Sample Data`) —
  characterized `Day Flight.mpg` / `Night Flight IR.mpg` via `ffprobe` + a TS/PMT/UL
  scan. Verified: ST 0601 UL key, BER long-form length, tag-2-first packet;
  KLV PID `0x1F1`, **`stream_type=0x06` + `KLVA`** (not `0x15`) — validates the
  gstklvplugin mux-signaling note against real samples.
* **Lint**: Promoted `Sample Data` from anticipated to current type in
  [`CONVENTIONS`](./CONVENTIONS.md) (first use). Added a `Sample Data` section
  to `index.md`.
* **Structure**: Added [`../planning/`](../planning/) (ROADMAP.md, PROGRESS.md)
  for transient planning/progress — distinct from this KB (evergreen) and
  `../docs/` (user-facing). Added a `# Decisions` section to
  [`CONVENTIONS`](./CONVENTIONS.md) with the `status:` vocabulary
  (proposed/accepted/superseded/deferred) and the open-fork → Decision
  lifecycle. Root `CLAUDE.md` now points at `planning/`.
* **Structure**: Created [`./decisions/`](./decisions/) subdir (with an ADR
  register, [`./decisions/index.md`](./decisions/index.md)) to group Decision
  concepts; root `index.md` Decisions section now points to it. Added a
  `# Subdirectories` policy to [`CONVENTIONS`](./CONVENTIONS.md): types stay
  flat by default, split only when noisy (~10+) or sub-structured (likely
  future split: `items/` for per-item 0601 concepts, deferred). Fork 7 in
  [`../planning/ROADMAP`](../planning/ROADMAP.md) narrowed to ADR
  format/numbering (location settled).
* **Process**: Added a `## Planning hygiene` directive to root `CLAUDE.md` —
  on a significant decision, write/update the ADR in `context/decisions/` and
  refresh ROADMAP (fork status + link) + PROGRESS; on implementing a
  significant change, refresh PROGRESS (and ROADMAP if a fork moves); routine
  ingests/lint stay in `log.md`. Ensures planning stays non-ephemeral.
* **Decision (proposed)**: Fork 1 →
  [`0001-build-system-and-cpp-standard`](./decisions/0001-build-system-and-cpp-standard.md)
  (proposed): CMake ≥3.20 + C++20 floor. Awaiting accept; flagged the
  embedded-toolchain assumption as the thing most likely to flip it to C++17.
  ROADMAP fork 1 → PROPOSED; ADR filename convention `NNNN-slug.md` adopted
  provisionally (fork 7 to formalize).
* **Decision (revised)**: Revised
  [`0001`](./decisions/0001-build-system-and-cpp-standard.md) on the Jetson
  constraint: JetPack 5 (Orin) ships GCC 9.3, which lacks `std::span` (needs
  GCC 10), so a C++20 span-based API won't build natively on stock JetPack 5.
  Floor revised C++20 → **C++17**; added multi-arch/cross-compile (x86_64 +
  aarch64) as a first-class CMake concern; byte views via `string_view` + a
  local view type. GPU noted as host-app/video-path, not core. Gate to
  acceptance: is JetPack-5-native required? (recommend yes → C++17).
* **Decision (accepted)**: [`0001`](./decisions/0001-build-system-and-cpp-standard.md)
  → accepted. User resolved the gate: **JetPack 6+ minimum, cross-compile only
  (no native Jetson builds)**. This removed the C++17 rationale → floor is
  **C++20** (`std::span` viable: header-only, cross-toolchain ≥ GCC 11). Build
  = CMake ≥3.20, multi-arch (native x86_64 + `aarch64` cross), C++23 deferred
  (local `Result<T>`). ROADMAP fork 1 → DECIDED.
* **Decision (proposed)**: Fork 2 →
  [`0002-license`](./decisions/0002-license.md) (proposed): permissive license
  — Apache-2.0 recommended (patent grant; codec/media-adjacent) with MIT as
  the lighter alternative. LGPL/AGPL/GPL rejected (user prefers permissive;
  AGPL blocks closed embedding). Awaiting SPDX pick (Apache-2.0 vs MIT) to
  accept. ROADMAP fork 2 → PROPOSED.
* **Decision (accepted)**: [`0002`](./decisions/0002-license.md) → accepted.
  License = **Apache-2.0** (permissive, patent grant). Canonical `LICENSE`
  added at repo root; per-file `SPDX-License-Identifier: Apache-2.0` headers
  when code lands. ROADMAP fork 2 → DECIDED.
* **Decision (proposed)**: Fork 3 →
  [`0003-project-name`](./decisions/0003-project-name.md) (proposed): resolve
  the `akrutsinger/libklv` namesake collision. Propose keep `libklv` +
  disambiguate (collision is with an inactive stub); alternative rename now
  (cheap — no code yet, docs-only find-replace; candidates `libmisbklv` /
  `misbklv` / `klvio` / `klvpp`). Defer rejected (renaming cheapest now).
  Awaiting pick. ROADMAP fork 3 → PROPOSED.
* **Decision (accepted)**: [`0003`](./decisions/0003-project-name.md) →
  accepted. Renamed the project `libklv` → **`libmisbklv`** (Option B) to
  eliminate the `akrutsinger/libklv` collision while rename is free (no code
  yet). `libklv_cpp` rejected (implies a port; C-ABI-incompatible). Doc
  self-refs renamed; external `akrutsinger/libklv` refs preserved. ROADMAP
  fork 3 → DECIDED. Repo dir rename left as a separate optional step.
* **Decision (proposed)**: Fork 7 →
  [`0004-adr-format`](./decisions/0004-adr-format.md) (proposed): formalize the
  de-facto ADR format (Nygard-style `NNNN-slug.md`, sequential by creation
  order — fork 7 = ADR 0004; frontmatter + body sections; register).
  Alternatives rejected: MADR (overhead), ad-hoc, external-tool. Full spec in
  [`CONVENTIONS`](./CONVENTIONS.md) § ADR format. Awaiting accept. ROADMAP
  fork 7 → PROPOSED.
* **Decision (accepted)**: [`0004`](./decisions/0004-adr-format.md) → accepted.
  ADR format formalized: Nygard-style `NNNN-slug.md`, sequential by creation
  order (fork 7 = ADR 0004), `status` frontmatter, `# Decision` heading always,
  body Context/Decision/Alternatives/Consequences/Assumptions/Citations,
  register in `decisions/index.md`. Spec lives in [`CONVENTIONS`](./CONVENTIONS.md)
  § ADR format. ROADMAP fork 7 → DECIDED. **Phase 1 (foundational decisions)
  complete.**
* **Decision (proposed)**: Fork 4 (split) → three proposed ADRs —
  [`0005`](./decisions/0005-klv-core-data-model.md) (KLV core data model,
  hybrid: tag+length+raw bytes + registry-driven typed view),
  [`0006`](./decisions/0006-tag-registry.md) (compiled-in `constexpr`,
  build-time codegen), [`0007`](./decisions/0007-error-and-c-abi.md) (local
  `Result<T>`; C ABI deferred). Four-layer architecture agreed. ROADMAP fork 4
  → PROPOSED. Awaiting accept.
* **Decision (accepted)**: Fork 4 —
  [`0005`](./decisions/0005-klv-core-data-model.md)/[`0006`](./decisions/0006-tag-registry.md)/[`0007`](./decisions/0007-error-and-c-abi.md)
  all accepted. Core architecture locked: hybrid data model (tag+len+raw
  bytes + registry-driven typed view, `std::span` zero-copy); compiled-in
  `constexpr` tag registry (build-time codegen); local `Result<T>` (no
  exceptions for routine errors); C ABI deferred (clean C++ core first).
  ROADMAP fork 4 → DECIDED. **Phase 2 (core architecture) design complete.**
* **Decision (proposed)**: Fork 5 →
  [`0008-media-backend-gstreamer`](./decisions/0008-media-backend-gstreamer.md)
  (proposed): single backend **gstreamer for v1** (library-style; `MediaBackend`
  interface kept; extract + insert incl. real-time via `appsrc` flow control;
  **PMT-rewrite component required** for `KLVA` insertion signaling — porting
  gstklvplugin's `tspmtrewrite`; ffmpeg deferred/optional). Project goal narrows
  from "gstreamer or ffmpeg" to "gstreamer (v1)". Awaiting accept. ROADMAP fork 5
  → PROPOSED.
* **Decision (accepted)**: [`0008`](./decisions/0008-media-backend-gstreamer.md)
  → accepted. Single **gstreamer backend for v1** (library-style;
  `MediaBackend` interface kept). Extract + insert incl. **real-time async
  (`0x06`)** via `appsrc` flow control. **PMT-rewrite = in-library gstreamer
  element** (option (c), porting gstklvplugin's `tspmtrewrite`; not a shipped
  plugin). Sync per-frame (`0x15`) deferred. ffmpeg deferred/optional. Project
  goal narrowed to "gstreamer (v1)"; `CLAUDE.md`/`README` reconciled. ROADMAP
  fork 5 → DECIDED.
* **Decision (deferred)**: Fork 6 →
  [`0009-st0604-deferred`](./decisions/0009-st0604-deferred.md) (deferred): ST
  0604 ES-timestamp layer deferred from v1 (secondary target; separate
  video-ES layer; NAL/SEI work comparable to the PMT-rewrite + same
  per-frame-sync shape as deferred sync-KLV; 0601 item 2 covers common
  correlation). v1 = 0601/0903 KLV via gstreamer. `CLAUDE.md`/`README`
  reconciled. ROADMAP fork 6 → DEFERRED. **Design complete — all forks
  resolved.**
* **Spike (Phase 3)**: Extracted the KLV elementary stream from
  [`data-samples`](./data-samples.md) `Day Flight.mpg` (`ffmpeg -map 0:1 -c copy
  -f data`; KLV PID `0x1F1`) and walked the first ST 0601 packet end-to-end
  (throwaway parser, not library code) to ground the pending descriptor-schema /
  encode ADRs in real bytes. Verified: UL key `060e2b34…`, BER long-form length
  `0x8191` (145 B value / 163 B packet), Item 2 first / Item 1 last (mandatory
  ordering), Precision Time Stamp = 2009-06-17T16:53:05Z, sensor lat/lon =
  54.68°, −110.17°, and the **16-bit BCC checksum recomputed and matched
  (`0x1C5F`)** — the encode-path checksum invariant is implementable as
  specified.
* **Lint (correction)**: Spike falsified a claim in [`st0601`](./st0601.md) —
  legacy core numeric items (lat/lon/angles, tags 5–15) use a **linear LDS map**
  (explicit int→float range; Item 13 = `int32` `-((2^31)-1)..(2^31)-1` → ±90°,
  `0x80000000`="Reserved"; §8.13), **not** IMAPB. IMAPB applies to the *extended*
  items (tags ~90+). Corrected `st0601` § Encoding to discriminate the two
  mapping kinds — the central input to the descriptor schema (a per-item
  mapping-kind + params field).
* **Decision (proposed)**: Fork 8 →
  [`0010-registry-descriptor-schema`](./decisions/0010-registry-descriptor-schema.md)
  (proposed): completes the descriptor field set [`0006`](./decisions/0006-tag-registry.md)
  punted. Flat `constexpr` `ItemDescriptor` (tag, name, `ValueKind` discriminator,
  `LengthSpec`, `MappingParams`, inline `SpecialValue[]`, `childRegistry`, flags)
  in per-registry tables (`TagEncoding` BER-OID vs 1-byte-UINT; UL key). `ValueKind`
  pins the [`0005`](./decisions/0005-klv-core-data-model.md) typed-view variant;
  codecs are a small shared set parameterized by the descriptor, not per-item.
  Authoring source-of-truth format + codegen tool left as a 0006 follow-on.
  ROADMAP fork 8 → PROPOSED.
* **Decision (accepted)**: [`0010`](./decisions/0010-registry-descriptor-schema.md)
  → accepted. Descriptor schema locked; unblocks the typed view, the shared codec
  set, and codegen. ROADMAP fork 8 → DECIDED.
* **Decision (proposed)**: Fork 9 →
  [`0011-encode-model`](./decisions/0011-encode-model.md) (proposed): the write
  half. **Owned builder** (coexists with the borrow-by-default read model),
  **bottom-up assembly** (a value is serialized before its BER length — no
  back-patching; nested sets/packs built into child buffers), `finalize()`
  validates mandatory items + emits Item 2 first / Item 1 checksum last
  (spike-verified BCC), returns an owned buffer moved to `appsrc`
  ([`0008`](./decisions/0008-media-backend-gstreamer.md)). Adds `EncodeError`
  variants to [`0007`](./decisions/0007-error-and-c-abi.md); reuses 0010's
  `MappingParams` for forward mapping. Draws the read-borrows/write-owns
  ownership boundary. Acceptance gate = byte-exact round-trip of the spike packet.
  ROADMAP fork 9 → PROPOSED.
* **Decision (accepted)**: [`0011`](./decisions/0011-encode-model.md) → accepted.
  Encode model locked: owned builder, bottom-up assembly, `finalize()` mandatory/
  ordering/checksum emission, owned-buffer handoff, `EncodeError` variants.
  ROADMAP fork 9 → DECIDED. **All forks resolved; the design backlog is clear —
  Phase 3 implementation (milestone 1: byte-exact round-trip) is unblocked.**
* **Decision (proposed)**: Fork 10 →
  [`0012-registry-codegen`](./decisions/0012-registry-codegen.md) (proposed): the
  [`0006`](./decisions/0006-tag-registry.md) codegen follow-on. **TOML**
  source-of-truth per registry (array-of-tables; per-item standard citation in
  comments) + a **Python generator** (`tools/gen_registry.py`, stdlib `tomllib`,
  validates tag-uniqueness / mapping-params / childRegistry / special-fit) emitting
  the [`0010`](./decisions/0010-registry-descriptor-schema.md) `constexpr` tables.
  **Generated C++ committed** (no Python build dep for consumers; reviewable),
  with a `regenerate-registry` target + CI drift check. JSON/YAML/hand-C++/
  build-time-gen rejected. ROADMAP fork 10 → PROPOSED.
* **Decision (accepted)**: [`0012`](./decisions/0012-registry-codegen.md) →
  accepted. Registry codegen locked: TOML source + Python generator + committed
  generated C++ + drift check. ROADMAP fork 10 → DECIDED. **All forks resolved.**
* **Milestone 1 (implementation)**: byte-exact round-trip landed. Real C++
  header-only core (`include/misbklv/`: `ber`/`types`/`codec`/`packet`/`builder`,
  `Result<T>`) per ADRs 0005/0007/0010/0011; TOML→`constexpr` generator
  (`tools/gen_registry.py`, `registry/uas0601.toml` →
  `src/registry/uas0601_tables.generated.hpp`) per ADR 0012; CMake + CTest per
  ADR 0001. `roundtrip_test` decodes `Day Flight.mpg`'s first packet, re-encodes
  (typed codecs for registered items, raw for the rest), recomputes the checksum,
  and reproduces all 163 bytes identically. Validates the descriptor schema +
  encode model against real bytes. Generator output byte-identical under `tomli`
  and the embedded fallback reader (drift check).
* **Milestone 2 (implementation)**: full-stream byte-exact round-trip.
  `stream_roundtrip_test` walks every packet in both samples and reconstructs the
  whole stream identically (Day Flight 6/6, Night Flight IR 18/18). Registry
  expanded to the full 25-tag sample set (26 descriptors; ranges from 0601.19
  §8). Two findings, both faithful to the ADRs: (1) **length-preserving
  reserialization** — wire length ≠ canonical length (`Day Flight` encodes Item
  22 Target Width as 4 B vs 0601.19 uint16), so `codec::encode` / `set()` are now
  length-parameterized (ADR 0011 updated); (2) **mandatory-enforcement opt-out**
  on `finalize()` for Report-on-Change packets (these samples are all full
  packets, so RoC trimming isn't yet exercised). Fixtures `dayflight.klv` /
  `nightflight_ir.klv` added.
* **Milestone 3 (implementation)**: 0903 nesting round-trip. Restored
  `RegistryId` + descriptor `child` (ADR 0010) + a `registry_for()` resolver;
  added the VMTI registry (`registry/vmti0903.toml`), recursive `parse_items`
  (bare TLV walk) and `LocalSetBuilder::serialize_items()` (nested value, no
  key/checksum). `nested_roundtrip_test` on a hand-authored fixture
  (`vmti_nested.klv` via `make_vmti_fixture.py`) — a 0601 packet nesting a VMTI
  LS in Item 74 — passes: childRegistry routing OK, nested + outer both
  byte-exact. Generator extended (`child`/`nested_ls`; variable-length uint from
  omitted length). Fixed a stale 16-vs-17-byte UL-key hex typo in unused TOML
  `ul_key` fields + the fixture script. Scalar-only; VTarget Series → M5.
 
* **Milestone 4 (implementation)**: real ST 1201 IMAPB codec. `ValueKind::IMAPB`
  now uses the Starting-Point-B mapping (`imapb_params`: bPow/dPow/sF/sR/Zoffset;
  `imapb_decode`/`imapb_encode`), split from the linear-LDS path in `codec.hpp`.
  `imapb_test` validates against the standards' own worked examples — ST 1201 §10
  Table 7 (IMAPB(-900,19000), L=3: enc(10.0)=0x038E00, plus 0.0/-900 vectors) and
  ST 0903 §10.1.11 (IMAPB(0,180), L=2: 12.5°=0x0640) — plus a full-range
  round-trip sweep. VMTI FoV items 11/12 (IMAPB) added to the registry + nested
  fixture for end-to-end coverage; 5th CTest case. VTarget Series → M5.
 
* **Milestone 5 (implementation)**: standalone VMTI. Restored `ul_key` to the
  `Registry` struct (generator emits a 16-byte key array + validates length);
  added `registry_by_key()` for UL-key → registry demux dispatch. New
  `standalone_roundtrip_test` on a hand-authored `vmti_standalone.klv` (VMTI UL
  key + items + checksum): dispatch resolves to VMTI (not 0601), decodes, and
  re-encodes byte-exact. Confirmed via ST 0903 §10.1.1 / **req 0903.6-119** that
  the standalone VMTI checksum is the ST 0601 16-bit-BCC algorithm over the whole
  LS (key..checksum-length) — so `finalize()` was already correct. **Both 0903
  variants now supported + tested: embedded (M3, Item 74) and standalone (M5).**
  6th CTest case. Cross-references evaluated per user request: **ATAK**
  (`VideoDropDownReceiver.java`) delegates KLV to the closed `com.partech.pgscmedia`
  lib — no parsing source to check against; **ImpleoTV MisbCore** docs are
  API-level (JSON), not byte-level, and ship no downloadable `vmtiPckt.bin` (that
  name is only a placeholder path in sample code) — ST 0903 §10 is the authority
  used. VTarget Series → M6.
* **Milestone 6 (implementation)**: VTarget Series (0903 Item 101). New
  `series.hpp` (`parse_vtarget_series` / `build_vtarget_pack` / `build_series`)
  for ST 0903 §9.1.3 Series of VTarget Packs (`[BER-OID targetId][LS items]`);
  new `VTARGET_0903` registry + `RegistryId::Vtarget0903`; Item 101 = `kind=pack`
  → child. `vtarget_roundtrip_test` on `vmti_vtarget.klv` (ST 0903 Figure 13:
  L=30 = two packs L=13/L=15) round-trips Series + packet byte-exact; 7th CTest
  case. **KLV core now structurally complete for 0601+0903** (scalars, nesting,
  packs/series, linear + IMAPB). Two bugs found + fixed against real structure:
  (1) IMAPB non-zero-`Zoffset` re-encode lost 1 LSB — added a few-ULP nudge to
  `imapb_encode` so decode∘encode is stable (imapb_test gained the IMAPB(-19.2,
  19.2) 10.0°=0x3A6667 vector); (2) `serialize_items()` skipped tag 1 as
  "checksum", wrong for embedded LSs where tag 1 is data (VTarget targetCentroid)
  — removed the assumption; checksum handling stays in `finalize()`. (Detail in
  PROGRESS.)
