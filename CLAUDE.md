# libmisbklv

C++ library to read and write MISB KLV data — ST 0601 (UAS Datalink Local
Set) + ST 0903 (VMTI) — from/to MPEG-TS containers via gstreamer (file or
stream; real-time insertion via `appsrc`). Video passthrough generates ST 0604
Precision Time Stamp SEI (ADR 0023); the rest of ST 0604 (ES-layer timestamp
reading, H.265 Nano, Commercial time code) and an ffmpeg backend are deferred —
see ADRs 0008/0009.

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

## Before every commit checklist

When you implement a significant change (new feature, milestone, decision):

1. ✅ Code changes staged
2. ✅ `context/log.md` updated (append dated entry)
3. ✅ `planning/PROGRESS.md` updated (Now/In-progress/Next reflect current state)
4. ✅ If decision made: ADR written, `context/decisions/index.md` updated

**Never commit code without updating the docs that describe it.** If you're about
to commit and haven't touched `log.md` or `PROGRESS.md`, stop and update them first.

What goes in each doc — and the rules behind this checklist — is **Planning
hygiene** below.

## Planning hygiene

Each doc has one job — keep them from re-narrating each other. **The *why* behind
each rule is in
[`context/workflow-rationale.md`](context/workflow-rationale.md) — read it before
relaxing a rule. A rule without its reason gets deleted the first time it's
inconvenient.**

- [`context/log.md`](context/log.md) — the **durable chronological record**:
  what landed when, milestone/decision detail, routine ingests/lint. History
  lives here, newest-first.
- [`context/decisions/index.md`](context/decisions/index.md) — the **decided
  register**: fork # ↔ ADR ↔ status. The single source of truth for what's
  decided.
- [`planning/PROGRESS.md`](planning/PROGRESS.md) — **present state only**
  (Now / In-progress / Next). Volatile; rewrite each session. No "Done" history
  (that's `log.md`); "Next" is the 1–3 *immediate* actions and **points to ROADMAP
  for the candidate backlog** — it doesn't re-list it.
- [`planning/ROADMAP.md`](planning/ROADMAP.md) — scope, phases, and the **single
  backlog** of open + candidate *forks* (a **fork** = a decision point that needs
  a choice — a fork in the road, not a git fork; see CONVENTIONS § Decisions).
  Defers the decided register (and the fork count) to `decisions/index.md`; never
  enumerates a fork range.

So:

- **On a significant decision** (a fork resolved or changed): write/update the ADR
  in [`context/decisions/`](context/decisions/index.md) per the lifecycle in
  [`context/CONVENTIONS.md`](context/CONVENTIONS.md) (status: proposed →
  accepted / superseded / deferred), add/update its row in the decided register
  (`decisions/index.md`), append **one thin** `log.md` line (chronology + link —
  the ADR owns the rationale; see CONVENTIONS § Decisions), and refresh
  PROGRESS's present state. Touch ROADMAP only if it opens/closes an *open* fork
  or shifts a phase.
- **On implementing a significant change**: append a `log.md` entry (the detail)
  and refresh [`planning/PROGRESS.md`](planning/PROGRESS.md)'s present state
  (Now / In-progress / Next) — a thin pointer, not a history. **Do this in the
  same commit** as the implementation: a doc updated "later" is wrong in between.
- **Closing scrubs the future**: when you resolve a fork or complete a
  candidate / deferred / "Next" item, *delete its forward-looking mentions*
  (ROADMAP candidates, PROGRESS "Next") — not just the present-state bullet you're
  editing. Open work has one home; a resolved item leaves it. (Forward-looking
  claims drift the same way history does — this is the future-tense half of
  "one job per doc".)
- **No live tallies in durable prose**: never bake a running count (test cases,
  item totals, "N of M done", **fork ranges like "1–N"**) into present-tense docs
  (PROGRESS, ROADMAP) or ADR bodies — it drifts. Say "all CTest cases green", not
  "19"; "none open, see the register", not "forks 1–17". A specific number belongs
  only in a dated `log.md` snapshot (history, frozen at write time). Same for any
  state that moves — describe the state, don't tally it.

## Commit messages

No `Co-Authored-By` trailer — keep history clean. When committing on the user's
behalf, write the message without the Claude co-authorship line.

## Repo layout

- `references/` — MISB standards (PDF + `.txt` extract). Source of truth.
- `context/` — agent knowledge bundle (maintain this).
- `docs/` — human-facing guides (terse).
- `data/` — sample MPEG-TS test vectors (Day Flight, Night Flight IR).
- `planning/` — live plan + progress (ROADMAP.md, PROGRESS.md). Read first for "what now".
