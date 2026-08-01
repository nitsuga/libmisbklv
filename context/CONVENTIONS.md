---
type: Conventions
title: Knowledge bundle conventions
description: How the context/ OKF bundle is structured, written, and maintained.
tags: [meta, okf, conventions]
timestamp: 2026-07-17T12:30:00Z
---

# Purpose

`context/` is the agent-facing knowledge base for libmisbklv, in
[OKF](https://github.com/GoogleCloudPlatform/knowledge-catalog/blob/main/okf/SPEC.md)
v0.1. It is the synthesis layer between immutable sources and the working
agent: knowledge is compiled here once and kept current, not re-derived each
session. The agent writes and maintains this directory; humans curate sources
and direct the work. On disagreement, the OKF spec wins over this doc.

# Layout

| Path | Role |
|------|------|
| `../references/` | Raw sources (standards PDFs + `.txt` extracts), append-only. Agent reads, and deposits new snapshots on a directed ingest; never edits one that's already there. |
| `context/` | This bundle. Agent-owned synthesis. |
| `../docs/` | Human-facing authored guides (terse). Not agent-maintained. |
| `../data/` | Sample MPEG-TS test vectors. |
| `../planning/` | Project management (ROADMAP, PROGRESS). Transient; human + agent read/write. Not OKF concepts. |

The `.txt` next to each standards PDF is a faithful text extract for grep/Read.
Cite a standard by name and section (`ST 0601 §6.3`), not by extract line
number — line numbers shift across revisions.

# Frontmatter

Every concept `.md` carries YAML frontmatter. Required: `type`. Recommended:
`title`, `description`, `tags`, `timestamp` (ISO 8601). Keep frontmatter
minimal; OKF tolerates unknown keys but they add noise.

# Type vocabulary

Open, not closed — new types may emerge. OKF tolerates unknown types, so an
emerging type breaks nothing; still, prefer an existing type if it fits. Lint
dedupes synonyms (see Operations).

Current types:

| Type | Use for |
|------|---------|
| `Standard Reference` | Per-MISB-standard synthesis (0601, 0604, 0903, 0107, 1201, …). |
| `Encoding Rule` | KLV mechanics (BER-OID tags, BER length, value encoding, Report-on-Change/ZLI). |
| `KLV Item` | A single 0601/0903 item: tag, length, units, decode method. |
| `Prior Art` | Existing KLV libraries/plugins analyzed as design input. |
| `Decision` | Architecture decision record — the *why* of a fork. |
| `Component` | A libmisbklv module (KLV core, gstreamer backend, ffmpeg backend). |
| `Sample Data` | Characterized input assets (sample streams, test vectors). |
| `Conventions` | Meta: how this bundle and the working method are run (this file, [`workflow-rationale.md`](./workflow-rationale.md)). |

Anticipated but not yet used: `Test Vector`; `Encoding Rule` and `KLV Item` have
index sections but no concepts yet. Add as needed.

# Decisions

A **fork** is a decision point — a branch in the design or plan that needs a
choice (the metaphor is a fork in the road; it has nothing to do with a git fork).
Forks are the unit the roadmap and the register track.

A `type: Decision` concept is an ADR: the *why* of a resolved (or proposed)
fork — context, alternatives considered, the choice, consequences. Carry a
`status:` frontmatter field:

| `status` | Meaning |
|---|---|
| `proposed` | Under deliberation; not yet adopted. |
| `accepted` | Resolved and in force. |
| `superseded` | Replaced by a later Decision (link it). |
| `deferred` | Parked; revisit later. |

Lifecycle: an open fork starts under **Open forks** in
[`../planning/ROADMAP.md`](../planning/ROADMAP.md) (`OPEN`). Once deliberated,
write the Decision in [`./decisions/`](./decisions/index.md) with
`status: proposed`; on resolution set `accepted`, add its row to the **decided
register** [`./decisions/index.md`](./decisions/index.md) (fork # ↔ ADR ↔
status), and refresh present state in
[`../planning/PROGRESS.md`](../planning/PROGRESS.md). ROADMAP no longer lists
decided forks — it defers to the register. Raw open questions live in
`../planning/`; only deliberated rationale lives here.

Record the decision in [`log.md`](./log.md) as a **single thin line** —
chronology + a link to the ADR. The ADR owns the rationale, so do **not**
restate the decision or its rejected alternatives in the log (that would put the
same content in three places). Add a *second* log line for one fork only on a
genuine cross-session gap (proposed one session, accepted a later one) or a
revision / supersede — not for a same-session propose→accept.

## ADR format

- **Filename:** `NNNN-slug.md` — 4-digit zero-padded, hyphenated lowercase slug.
  Numbering is sequential by **creation order** (not roadmap-fork number; they
  diverge — fork 7 → ADR 0004); monotonic; never reused (a superseded ADR keeps
  its number, the superseder takes the next).
- **Frontmatter:** `type: Decision`, `title`, `status`, `tags` (include
  `decision`, a topic tag, `phase-N`), `timestamp` (ISO 8601), optional `fork:`
  (roadmap fork # for traceability).
- **Body:** `# Context` → `# Decision` → `# Alternatives considered` →
  `# Consequences` → `# Assumptions / open questions` → `# Citations`
  (specialized sections allowed between). Use `# Decision` always — `status`
  carries proposed/accepted, so no header churn on accept.
- **Register:** [`./decisions/index.md`](./decisions/index.md) lists
  `Fork | ADR | Status`; updated on every status change.
- Decided in [`./decisions/0004-adr-format.md`](./decisions/0004-adr-format.md).

# Subdirectories

Types stay flat in `context/` by default; the root `index.md` groups by type.
Split a type into its own subdir when flat-listing grows noisy (~10+ files) or
develops sub-structure, and give the subdir its own `index.md`. `decisions/`
is pre-created because ADRs accumulate and supersede (a register is useful
immediately). The likely future split is `items/` if per-item 0601 concepts are
ever ingested (deferred — see [`../planning/ROADMAP.md`](../planning/ROADMAP.md)).

# Linking

Sibling-relative markdown, resolved from `context/`: `[ST 0601](./st0601.md)`,
and `../planning/…` / `../references/…` for targets outside the bundle. This is
what the bundle uses throughout, and it resolves for a reader on GitHub, in an
editor, and for an agent following paths alike.

Never root-absolute (`](/st0601.md)`): it resolves against the *site* root, not
the bundle, so it works nowhere. A lint on 2026-07-27 fixed ~68 of these.

`[[slug]]` wikilinks appear in a few places and are understood, but are not the
house style here — they do not render on GitHub.

Relationship semantics — references, nests-in, depends-on — live in the
surrounding prose; the link itself is untyped.

**Broken links are errors here.** OKF *consumers* must tolerate them — a
dangling link may be not-yet-written knowledge — but that's a robustness rule
for readers, not license for this bundle to ship them:
[`link-check`](../.github/workflows/link-check.yml) fails the build on one.
When a concept isn't written yet, name it in prose without linking, or write
the stub; the lint's "concepts mentioned but not written" pass (§ Operations)
is what turns those into links later.

# Reserved files

`index.md` — directory listing for progressive disclosure. No frontmatter
except `okf_version` at the bundle root. `log.md` — chronological change
history, newest first, ISO `YYYY-MM-DD` headings. Both agent-maintained.

# Operations

**Ingest.** A new source (a standards PDF, a prior-art repo) enters
`../references/` or is fetched. Read it, extract key facts, write a concept
doc, update relevant existing concepts and cross-links, add an `index.md`
entry, append a `log.md` line. One source may touch several docs.

**Query.** Read `index.md` first to find relevant concepts, drill in,
synthesize with citations back to `../references/` or external URLs. File
valuable query results back as new concepts so explorations compound.

**Lint.** Periodically check for: contradictions between docs, stale claims
superseded by newer sources, **forward-looking claims a later change resolved**
(an "open decision" / "candidate" / "Next" item that's since been decided or
done — the future-tense analog of a stale claim), orphan concepts with no inbound
links, concepts mentioned but not written, broken-link targets worth creating,
and type drift (synonyms / casing). Tidy the `type` list when synonyms appear.
No trigger fires the lint automatically — run it periodically (e.g. when closing
a fork, or before a release). Broken *internal* links are the one part CI
catches ([`link-check`](../.github/workflows/link-check.yml)); the rest is a
read-through.

# Source discipline

`../references/` is **append-only**. Never edit, reformat, or delete a file
that's already there — the immutable standards PDFs are the source of truth,
and this bundle is the curated, cross-linked reading of them. Adding is
different from editing: when the user directs an ingest (e.g. ST 0603.5, added
2026-07-27), deposit the new standard's PDF and `.txt` extract there as a
faithful snapshot (§ Operations — Ingest), then leave it alone forever.
"Immutable" governs each snapshot, not the directory's file count.

Correcting a source you believe is wrong is *never* an edit there: the
correction is knowledge, so it belongs in a `context/` concept that cites the
original and says where it departs from it.
