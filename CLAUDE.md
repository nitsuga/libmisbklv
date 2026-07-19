# libmisbklv

C++ library to read and write MISB KLV data — ST 0601 (UAS Datalink Local
Set) + ST 0903 (VMTI) — from/to MPEG-TS containers via gstreamer (file or
stream; real-time insertion via `appsrc`). ST 0604 (ES-layer timestamps) and
an ffmpeg backend are deferred — see ADRs 0008/0009.

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

Each doc has one job — keep them from re-narrating each other:

- [`context/log.md`](context/log.md) — the **durable chronological record**:
  what landed when, milestone/decision detail, routine ingests/lint. History
  lives here, newest-first.
- [`context/decisions/index.md`](context/decisions/index.md) — the **decided
  register**: fork # ↔ ADR ↔ status. The single source of truth for what's
  decided.
- [`planning/PROGRESS.md`](planning/PROGRESS.md) — **present state only**
  (Now / In-progress / Next). Volatile; rewrite each session. Do **not** grow a
  "Done" history here — that's `log.md`.
- [`planning/ROADMAP.md`](planning/ROADMAP.md) — scope, phases, and **open**
  (undeliberated) forks. It defers the decided register to
  `decisions/index.md`.

So:

- **On a significant decision**: write/update the ADR in
  [`context/decisions/`](context/decisions/index.md) per the lifecycle in
  [`context/CONVENTIONS.md`](context/CONVENTIONS.md) (status: proposed →
  accepted / superseded), add/update its row in the decided register
  (`decisions/index.md`), append **one thin** `log.md` line (chronology + link —
  the ADR owns the rationale; see CONVENTIONS § Decisions), and refresh
  PROGRESS's present state. Touch ROADMAP only if it opens/closes an *open* fork
  or shifts a phase.
- **On implementing a significant change**: append a `log.md` entry (the detail)
  and refresh [`planning/PROGRESS.md`](planning/PROGRESS.md)'s present state
  (Now / In-progress / Next) — a thin pointer, not a history.
- **No live tallies in durable prose**: never bake a running count (test cases,
  item totals, "N of M done") into present-tense docs (PROGRESS, ROADMAP) or ADR
  bodies — it drifts. Say "all CTest cases green", not "19". A specific number
  belongs only in a dated `log.md` snapshot (history, frozen at write time). Same
  for any state that moves — describe the state, don't tally it.

## Commit messages

No `Co-Authored-By` trailer — keep history clean. When committing on the user's
behalf, write the message without the Claude co-authorship line.

## Repo layout

- `references/` — MISB standards (PDF + `.txt` extract). Source of truth.
- `context/` — agent knowledge bundle (maintain this).
- `docs/` — human-facing guides (terse).
- `data/` — sample MPEG-TS test vectors (Day Flight, Night Flight IR).
- `planning/` — live plan + progress (ROADMAP.md, PROGRESS.md). Read first for "what now".
