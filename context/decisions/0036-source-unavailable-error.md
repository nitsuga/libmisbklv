---
type: Decision
title: A live source that is absent right now is SourceUnavailable, not Unsupported
decision_status: accepted
tags: [decision, api, backend, streaming, errors]
generated:
  by: claude/opus-5
  at: 2026-08-29T06:01:54Z
sources:
  - id: libmisbklv-61
    resource: https://github.com/nitsuga/libmisbklv/issues/61
    title: "insert: Unsupported conflates a transient source outage with permanent configuration failures"
  - id: parrot-to-klv-103
    resource: https://github.com/nitsuga/parrot-to-klv/pull/103
    title: parrot-to-klv#103 — reconnect mode, the consumer that has to tell the two apart
fork: 33
---

# Context

`Error::Unsupported` was returned from sixteen sites in the GStreamer backend
for conditions a caller needs to tell apart.[^libmisbklv-61] Two of them mean
*the live source is not there right now*: an unreachable RTSP peer, and an RTSP
source that produces no video pad within `kLivePadTimeout`. The rest mean *this
configuration cannot work*: a `pipeline:` description that fails to parse, a
`pipeline:` carrying a demuxer, a container this library does not demux, an
unsupported `VideoSourceKind`, and — via `sink ? Error::Backend :
Error::Unsupported` in `open_insert` — a missing sink element factory.

A consumer that retries has to distinguish them, and could not. parrot-to-klv's
reconnect mode polls a live source between sorties and starts a fresh pipeline
when it returns; it must retry an aircraft that is powered off and must not
retry a configuration that will never run.[^parrot-to-klv-103] It resorted to
inferring the difference from the source scheme — retry only when the URI is
`rtsp://` — which left one case undecidable: a missing sink element factory with
an RTSP source is indistinguishable from a powered-off aircraft, since both fail
identically on every attempt from the first, with no state to separate them.

A downstream probe cannot recover the information. Opening a sink at startup to
test it is unsafe for `udp:`/`srt:` sinks, where having no peer yet is a normal
condition rather than a fault, so the probe would produce false fatals on
exactly the live outputs it was meant to protect. The distinction is only
available where the failure happens.

# Decision

Add `Error::SourceUnavailable`, **appended** to the enum so every existing
numeric code keeps its value — the same treatment `TypeMismatch`,
`ResourceLimit`, and `ReadOnly` received (ADR 0007, ADR 0011).

- `SourceUnavailable` means a **live source that is absent right now**. Only
  `prepare_rtsp_branch` returns it, and only after classifying *why* it failed.
  A caller polling for a stream to come back retries on this code and nothing
  else.

  Classification matters, because the RTSP branch's two failure paths each
  cover both meanings. A bus error is routed first by **where it came from**:
  the branch watches the whole pipeline bus, so an error posted by the muxer or
  the sink arrives alongside one from `rtspsrc`, and only the latter says
  anything about the source. A message from outside the branch is `Backend` —
  an output fault, never an absent source. Classifying a full disk as "absent"
  would have a polling consumer retry it forever, which is the exact failure
  this ADR exists to prevent.

  A message from the branch is then routed by its **RTSP status** when it has
  one. `rtspsrc` folds most non-2xx responses onto `RESOURCE/READ` and 404 onto
  `RESOURCE/NOT_FOUND`, so the domain and code alone would read a bad path, an
  unsupported transport, or a rejected request as an absent source. A status at
  all means the server answered — it is reachable, and the request was refused
  on its merits — so only a server-side 5xx is transient. The status lives in
  the message's error details under `rtsp-status-code`, written by `rtspsrc`
  as `G_TYPE_UINT` — read it with the matching getter, since the typed
  getters report failure on a mismatch instead of converting, and a silent 0
  turns the whole rule off.

  With no status the failure never reached the protocol, which is the
  refused-connection case, and the `GError` domain decides.
  `GST_RESOURCE_ERROR` yields `SourceUnavailable` only for its **read-side**
  codes — `NOT_FOUND`, `OPEN_READ`, `OPEN_READ_WRITE`, `READ`, and `BUSY`, a
  camera whose client slot is taken. Write-side codes (`OPEN_WRITE`, `WRITE`,
  `NO_SPACE_LEFT`, `CLOSE`) and `NOT_AUTHORIZED` are permanent: an output that
  cannot be written is not an absent source, and credentials do not become
  correct by waiting. The code split is deliberate redundancy behind the origin
  check — either alone would keep a disk-full error out of the retry path. `GST_STREAM_ERROR` and `GST_CORE_ERROR` mean
  the server answered and its media cannot be handled by this build — an
  unsupported encoding, a missing depayloader — and stay `Unsupported`. An
  unrecognized domain is treated as permanent, because an unrecognized error
  must not become an infinite retry in a consumer that polls on this code.

  The no-video-pad timeout is split the same way, on whether `rtspsrc`
  announced any pad at all. A pad means the server answered and described
  streams we cannot carry: permanent. No pad means nothing answered: absent.
- `Unsupported` keeps its documented meaning — not implemented in this backend
  or this configuration — and is now unambiguously **permanent**. An
  unparseable `pipeline:`, a demuxer-bearing `pipeline:`, an undemuxable
  container, an unknown source kind, and a missing element factory all stay
  here.
- `Backend` is unchanged: a pipeline or I/O fault.

The split is chosen in this direction, rather than carving a separate code out
for the missing-factory case, because "not back yet" is the state a caller
actually polls on. One code that means *retry me* is more useful than several
that mean *do not*, and it leaves `Unsupported` with a single coherent meaning
instead of two.

# Alternatives considered

- **Add `Unavailable` for a missing element factory instead** — rejected. It
  fixes the sink residual but leaves `Unsupported` still covering both an
  unreachable RTSP source and an unparseable `pipeline:`, so a consumer would
  still have to infer intent from the source scheme. Half the cleanup for the
  same API cost.
- **Leave the classification downstream** — rejected; it cannot be done
  correctly there. The scheme heuristic is approximate by construction, and the
  sink case is undecidable without information only the backend has.
- **A startup sink probe in the consumer** — rejected; unsafe for live sinks,
  where opening with no peer is not a fault. See Context.
- **Reuse `Backend` for the transient case** — rejected; `Backend` means a
  fault occurred, while an aircraft that has not taken off yet is not a fault,
  and callers already treat `Backend` as terminal.
- **A richer result type carrying a retryable flag** — rejected as
  disproportionate. One appended enumerator answers the question callers are
  asking, and the enum is already the vocabulary they branch on.

# Consequences

- A consumer can branch on the error code alone. parrot-to-klv#103's
  scheme-based inference collapses to `r.error() == Error::SourceUnavailable`,
  and its documented residual disappears — a missing sink plugin now reports a
  permanent failure and stops the run instead of retrying forever.
- **Behavior change for existing callers**: an unreachable RTSP source now
  returns `SourceUnavailable` where it returned `Unsupported`. Code that
  branches on `Unsupported` for that case must be updated. Pre-1.0, and the
  library's own tests are updated in the same commit, but a downstream pin bump
  carries it.
- Numeric codes are preserved, so the C ABI and any persisted value stay valid;
  only the new enumerator is added at the end.
- The error vocabulary now distinguishes *transient* from *permanent* for live
  sources only. Nothing else in the library reports transience, and no other
  site should return this code without the same justification.

# Assumptions / open questions

- Read-side bias: a write failure originating *inside* the RTSP branch — an
  `rtspsrc` that cannot send over an interleaved TCP connection — is classified
  permanent, though it may be transient. That is the conservative direction, and
  a peer that accepted a connection and then failed on write is a fault rather
  than an absence. Revisit if it proves wrong in the field.
- The classifier is keyed on GStreamer error domains, which are stable API but
  broader than the cases enumerated here. New domains default to permanent,
  which fails safe for a polling consumer but will silently mark a genuinely
  transient future case as fatal. Revisit if a live transport is added.
- Only RTSP can currently be transiently absent. A future live input that can
  also be temporarily down — a network video source that is not RTSP — should
  return `SourceUnavailable` from its own branch rather than growing another
  code.
- The extraction path (`extract`) has its own live sources and is untouched
  here. If a consumer ever needs to poll extraction for a source coming back,
  the same distinction will be wanted there, and this code is the one to use.

# Citations

[^libmisbklv-61]: [libmisbklv#61](https://github.com/nitsuga/libmisbklv/issues/61)
    — the survey of all sixteen `Unsupported` sites and the split proposed here.
[^parrot-to-klv-103]: [parrot-to-klv#103](https://github.com/nitsuga/parrot-to-klv/pull/103)
    — the reconnect mode whose retry loop needs the distinction, and the
    scheme-based workaround this replaces.
