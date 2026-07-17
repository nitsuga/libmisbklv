# libklv

C++ library to read and write MISB KLV data — ST 0601 (UAS Datalink Local
Set), ST 0903 (VMTI), ST 0604 (ES-layer timestamps) — from/to MPEG-TS
containers, via gstreamer or ffmpeg, file or stream.

## Agent knowledge base

The project's working knowledge lives in `context/` as an
[OKF](https://github.com/GoogleCloudPlatform/knowledge-catalog/blob/main/okf/SPEC.md)
v0.1 bundle.

- Read [`context/index.md`](context/index.md) first — the bundle catalog.
- [`context/CONVENTIONS.md`](context/CONVENTIONS.md) — frontmatter, type
  vocabulary, ingest/query/lint rules. Read before editing `context/`.
- `context/` is agent-owned. `references/` is immutable — read, never modify.
- For the live plan and status, read [`planning/PROGRESS.md`](planning/PROGRESS.md)
  and [`planning/ROADMAP.md`](planning/ROADMAP.md).

## Planning hygiene

Keep `planning/` current as the project moves — it's the non-ephemeral record
of decisions and status, not chat.

- **On a significant decision** (a fork resolved or changed): write/update the
  ADR in [`context/decisions/`](context/decisions/index.md) per the lifecycle
  in [`context/CONVENTIONS.md`](context/CONVENTIONS.md) (status: proposed →
  accepted / superseded), then update the fork's row in
  [`planning/ROADMAP.md`](planning/ROADMAP.md) (status + Decision link) and
  [`planning/PROGRESS.md`](planning/PROGRESS.md).
- **On implementing a significant change**: update
  [`planning/PROGRESS.md`](planning/PROGRESS.md) (done / in-progress / next);
  if it advances or closes a roadmap fork, update ROADMAP too.
- Routine ingests / lint go to [`context/log.md`](context/log.md), not PROGRESS.

## Repo layout

- `references/` — MISB standards (PDF + `.txt` extract). Source of truth.
- `context/` — agent knowledge bundle (maintain this).
- `docs/` — human-facing guides (terse).
- `data/` — sample MPEG-TS test vectors (Day Flight, Night Flight IR).
- `planning/` — live plan + progress (ROADMAP.md, PROGRESS.md). Read first for "what now".
