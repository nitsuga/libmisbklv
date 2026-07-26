---
type: Decision
title: "\"No output file on failure\" spans the whole insert session"
status: accepted
tags: [decision, backend, muxing, insertion, phase-3]
timestamp: 2026-07-26T10:30:00Z
fork: 20
---

# Context

[`0020`](./0020-video-passthrough.md) established that **a failed
`open_insert()` leaves no output file**: a `filesink` creates its file the moment
the pipeline leaves `NULL`, so a failure in `PAUSED` (a readable source with no
video stream) would otherwise leave a zero-byte `.ts` that reads as output to
anything scanning the directory. `open_insert` unlinks it, under a
`sink_preexisted` guard so a file it did not create is never deleted.

The guarantee stopped at the end of that function. Failures one step later leak
exactly the same file, and they are reachable by an ordinary consumer mistake —
a source whose video track is *declared but unparseable*:

```
open_insert()  -> OK      (a video pad appears from the sample entry)
push() x N     -> OK
finish()       -> Error   (the pipeline errors out as it drains)
```

Reproduced with an MP4 whose `avc1` sample entry has no `avcC`: `h264parse`
refuses the caps, the muxer cannot create a handler for the stream, and the
whole thing fails only at EOS — leaving a zero-byte `.ts`. `parrot-to-klv` hit
this while generating fixtures and carries a local workaround: remove the output
itself, under its own "only if this run created it" guard. Every consumer that
cares would have to duplicate it. The docs, meanwhile, already read as though the
guarantee covered the session.

# Decision

**The guarantee is a property of the insert session, not of `open_insert`.** For
a `file:` sink, an output file exists only if `finish()` returned ok.

- `open_insert` hands the `Inserter` the sink path **only when this call created
  it** (`sink_preexisted ? "" : sink_path`). The Inserter therefore holds a path
  it is allowed to delete, or nothing at all — the "never delete a file we did
  not create" rule is enforced by construction rather than by a flag every call
  site must remember to check.
- A **failing `finish()`** unlinks that file, after the pipeline is in `NULL` so
  the sink has closed it. A **successful `finish()`** clears the path: that file
  is the output now and nothing may remove it.
- **Destruction without a successful `finish()`** unlinks it too. An abandoned
  session has not produced output, only an unfinalized file — a `.ts` whose
  muxer never wrote its final state. Treating "the caller gave up" as success
  would leave exactly the artifact this ADR is about.
- Unchanged: a **pre-existing** file at the sink path is never deleted, and its
  old contents are still lost the moment the sink opens it (a file sink
  truncates, on the success path too). That asymmetry is documented, not fixed —
  avoiding it means muxing to a temporary and renaming, which is a bigger change
  than the leak justifies.

# Alternatives considered

- **Leave it to consumers.** Rejected: it is the same three lines in every
  consumer, and each has to re-derive the "only if this run created it" subtlety
  that the library already got right once.
- **Only clean up on a failing `finish()`, not on destruction.** Half the fix: a
  `push()` that fails mid-stream, or a consumer that returns early on an error,
  never reaches `finish()` and leaks the file. `KlvSink` makes this the common
  shape — its destructor runs whether or not `close()` was called.
- **Mux to a temporary file and rename on success.** The strongest form — it
  would protect a pre-existing file's contents too — but it changes the sink
  grammar (a temp path must be choosable and on the same filesystem) and only
  works for `file:` sinks. Out of proportion to the defect.
- **Widen the guarantee to `udp`/`srt` sinks.** Meaningless: bytes already sent
  cannot be recalled. The guarantee is file-sink-specific and says so.

# Consequences

- `open_insert` + `push` + `finish` is now all-or-nothing for a file sink: an
  error result means there is no output at that path (unless something was
  already there).
- `KlvSink` inherits it — an output file exists only if `close()` returned ok,
  including when the sink is destroyed without a `close()` at all.
- `parrot-to-klv` can drop its local cleanup; the guard it duplicates now lives
  in the library.
- `gst_video_insert_test` covers both new paths: a session abandoned without
  `finish()` (always run), and a late failure from an MP4 whose `avcC` box the
  test blanks in the copy it remuxed for the `qtdemux` case — open succeeds,
  `finish()` fails, no file is left. Both re-check the pre-existing-file guard.
- A caller who *wants* the partial file that a broken session wrote no longer
  gets it. Nobody asked for that, and an unfinalized `.ts` is not usable output.

# Assumptions / open questions

- **`std::remove` failures are ignored** (as in `open_insert`). If the file
  cannot be removed, there is nothing better to report through a `Result` that
  is already carrying the real error.
- The Inserter deletes a path it recorded at open time. A caller that moves or
  replaces the file underneath a running session gets what it asked for.

# Citations

[1] [`0020`](./0020-video-passthrough.md) — the open-path half of this
    guarantee, and the `sink_preexisted` pattern extended here.
[2] [`0013`](./0013-media-backend-interface.md) — the `Inserter` session
    (`push`/`finish`) whose lifetime this ADR attaches the guarantee to.
