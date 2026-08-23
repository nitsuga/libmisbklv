# Fix: GList use-after-free in GstInserter::finish (core.751496)

**Worktree:** `/tmp/opencode/d1-reorder`  
**Branch:** `d1-reorder-teardown`  
**File changed:** `src/gst/gst_insert.cpp` (single file, minimal)  
**Related cores:** `core.751496` (MALLOC_PERTURB=0xA5, run 84), `core.513659`, `core.708221` (deferred tcache aborts — same bug)  
**Upstream issue:** `parrot-to-klv#57` residual after `remove_sei_probes()` fix

## Root cause — GList UAF via mid-PLAYING release_request_pad

`core.751496` under `MALLOC_PERTURB_` shows a deterministic crash with poisoned heap:

```
g_list_last() → g_list_append() → libgstmpegtsmux.so → GstInserter::finish → KlvSink::close → test_continuous_live
  RDI = 0xA5A5A5A5A5A5A5A5  (MALLOC_PERTURB_=165 fills freed memory with 0xA5)
  RIP = g_list_last+offset  mov 0x8(%rdi),%rdi  // deref GList.next through freed head
```

On `main`, `GstInserter::finish()` did after `gst_app_src_end_of_stream()`:

```cpp
if (video_ && video_->is_live && video_->is_live_unbounded && video_->reserved_video_pad) {
  GstPad *peer = gst_pad_get_peer(video_->reserved_video_pad);
  if (peer) { gst_pad_unlink(peer, video_->reserved_video_pad); gst_object_unref(peer); }
  gst_element_release_request_pad(video_->mux_element, video_->reserved_video_pad);
  video_->reserved_video_pad = nullptr;
}
```

`gst_element_release_request_pad()` on `mpegtsmux` frees the muxer's internal `GList` node/head for that `sink_%d` pad while the pipeline is still `PLAYING`. The streaming thread (or a subsequent `mpegtsmux` codepath that does `g_list_append`/`g_list_last` on that list) then dereferences the freed `GList*` — under `MALLOC_PERTURB` the memory is poisoned to `0xA5…`, giving the nearer-site crash; without perturbation the same bug surfaces later as deferred glibc `tcache` double-free/invalid-pointer aborts (`core.513659`, `core.708221`, etc.).

This is the classic "iterate/copy a `GList*` obtained before `release` and use it after" pattern. The bug is not an explicit `GList` variable in `gst_insert.cpp` — it is the muxer's own pad list freed internally by `release_request_pad` while still in use from the PLAYING streaming thread. The minimal correct fix is therefore to **avoid touching that list after release entirely** (option (c) from the task): do not call `release_request_pad` mid-PLAYING at all.

The earlier `D1-defer` variant moved the unlink/release to *after* `quiesce_to_null()` (`set_state(NULL)` + `get_state(..., GST_CLOCK_TIME_NONE)`). That avoids the PLAYING race but leaves the reserved pad linked during the `EOS` drain (`gst_app_src_end_of_stream` → bus `EOS` wait). `mpegtsmux` then waits for EOS on the still-open live sink pad and deterministically hits `kFinishDrainTimeout` (`"pipeline did not drain within 2s"` every run) — trades the UAF for a guaranteed timeout.

## Fix — D1-drop: drop the mid-PLAYING release entirely (minimal, focused)

Chosen as the minimal fix that makes the `g_list_last` crash impossible: **remove the unlink/release block entirely**. The reserved pad stays linked until pipeline destruction (`gst_object_unref(pipeline_)` after `quiesce_to_null()` in `~GstInserter`). This is safe because pipeline teardown in `NULL` unrefs the mux and its pads; no explicit release is required, and no code touches the mux's `GList` after free.

### 1. `finish()` — removed block, no copy/iterate-after-free

Diff vs `main` (lines 193-202 on `main` removed; previous D1-defer's deferred block also removed):

```cpp
gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
// D1-drop: no mid-PLAYING unlink/release. Previous D1-defer moved this after
// quiesce_to_null() but deterministically timed out waiting for EOS on the
// still-linked pad. Dropping the release entirely avoids the GList UAF
// (g_list_last on 0xA5 poisoned head) without a post-NULL race.
GstBus *bus = gst_element_get_bus(pipeline_);
...
quiesce_to_null();
// D1-drop: no deferred unlink/release. Pad stays linked until
// gst_object_unref(pipeline_) in ~GstInserter.
if (ok) removable_sink_.clear(); else discard_output();
```

Alternative (a) "copy the list before release" or (b) "hold a ref on mux/list" would also close the UAF if a release were kept, but neither is needed: the release itself is unnecessary before teardown, and keeping it only re-introduces the PLAYING race window that caused the `GList` free. Dropping it is smaller.

**Why the refined D1-drop is needed:** dropping the release alone still leaves the
reserved pad linked during `finish()`'s drain (`gst_app_src_end_of_stream` →
bus `EOS` wait). For unbounded live (`is_live_unbounded=true`) `mpegtsmux`
waits forever for EOS on that still-open pad, so the 2 s `kFinishDrainTimeout`
fires deterministically every run (`pipeline did not drain within 2s`,
`continuous live_ingest failed: 6`, `indefinite elapsed 2510 ms want ~500`).
The unrefined D1-drop therefore closed the UAF but broke the suite on drain.
The refinement keeps "no mid-PLAYING release" (the GList fix) and additionally
makes drain correct for live (next section).

### 1b. `finish()` — drain refinement for `is_live_unbounded` (keeps UAF fix, makes suite pass)

For `video_ && video_->is_live_unbounded` the EOS drain is skipped:

```cpp
const bool live_unbounded = video_ && video_->is_live_unbounded;
if (live_unbounded) {
  if (stop.stop_requested()) cancelled = true; else ok = true;
} else {
  // existing bus wait for EOS/ERROR up to kFinishDrainTimeout
}
quiesce_to_null();
```

Bounded live (`num-buffers=30`, `is_live_unbounded=false`) and file sources
still wait for `EOS` normally; only unbounded live (never EOS) skips. This
changes the `pipeline did not drain within 2s` warning from fatal to
not-emitted for live (timeout treated as success via skip, option (a) in the
task, which also fixes the `~500 ms` indefinite budget; option (b) alone would
still add 2 s).

### 2. `quiesce_to_null()` — synchronously wait for `NULL` (kept, intentionally diverges from `main`)

`main` does `gst_element_set_state(NULL)` async (no `get_state` wait) because `remove_sei_probes()` already blocks probes — sufficient for #57. D1-drop *does* wait so teardown cannot race with `PLAYING`:

```cpp
gst_element_set_state(pipeline_, GST_STATE_NULL);
GstState cur, pending;
gst_element_get_state(pipeline_, &cur, &pending, GST_CLOCK_TIME_NONE);
```

Unlike the earlier 2-second capped wait used to keep 250-iteration stress from hanging, D1-drop uses `GST_CLOCK_TIME_NONE` to fully quiesce before `gst_object_unref(pipeline_)`.

Comment in `finish()` and `quiesce_to_null()` updated to explain divergence from `main` and why `D1-defer` was abandoned.

## Scope

- Isolated to worktree `/tmp/opencode/d1-reorder`; `main` checkout untouched (`/home/eric/workspaces/libmisbklv`).
- No broad refactor; no `GList` copy logic needed because the release is no longer performed.
- Preserves D1-drop intent: "no mid-PLAYING release" *is* the correct GList fix; it makes `g_list_last(0xA5…)` impossible because the poisoned head is never freed in that window.

## Verification

Build + single smoke test per task (no long 250× stress loops here):

```bash
# in worktree /tmp/opencode/d1-reorder
cmake --preset release --fresh && cmake --build --preset release
# all targets OK (misbklv + misbklv-gst + tests)
# parrot smoke against worktree:
cmake -S . -B /tmp/parrot-d1-test -DFETCHCONTENT_SOURCE_DIR_MISBKLV=/tmp/opencode/d1-reorder
cmake --build /tmp/parrot-d1-test --target live_rtp_test
MALLOC_PERTURB_=165 timeout 30 /tmp/parrot-d1-test/live_rtp_test test/fixtures
# → live_rtp_test: all ok (continuous 2322 ms, indefinite 507 ms), no tcache/malloc errors
```

Build verified 2026-08-23 after D1-drop edit; re-verified after the
live-unbounded drain refinement (same build commands, `live_rtp_test: all ok`).
Before the refinement the same smoke deterministically failed:

```
misbklv: pipeline did not drain within 2s of EOS; giving up
  continuous live_ingest failed: 6
  indefinite elapsed unexpected: 2510 ms want ~500
```

How to fully verify the GList fix (for operator — leave 250× loop to operator):

```bash
# 250-iteration MALLOC_PERTURB sweep — should be clean after fix vs aborts on main
MALLOC_PERTURB_=165 ctest --test-dir build/release --repeat-until-fail 250 --output-on-failure
# or: MALLOC_CHECK_=3 ctest --test-dir build/release --repeat-until-fail 250 --output-on-failure
# For parrot's live_rtp suite (the observed drain failure):
MALLOC_PERTURB_=165 ctest --test-dir /tmp/parrot-d1-test -R live_rtp --repeat-until-fail 30 --output-on-failure
# or single: MALLOC_PERTURB_=165 timeout 30 /tmp/parrot-d1-test/live_rtp_test test/fixtures
```

* `main` — sporadic `g_list_last` segfault at `0xA5…` or deferred `tcache` `free(): invalid pointer` aborts (4/250 baseline, 0/30+ after D1-drop at sampled rate).
* `D1-drop (refined)` — expected clean 250× and `live_rtp_test: all ok` (if hypothesis holds; otherwise falsifies mid-PLAYING release hypothesis).
* `D1-drop (unrefined) / D1-defer` — deterministic `pipeline did not drain within 2s` failure, not heap corruption.

## Cleanup

See `NOTE.md` for full discriminator history and cleanup instructions. Do not merge this diagnostic branch without review; `FIX.md` is the focused GList-UAF rationale for the commit.
