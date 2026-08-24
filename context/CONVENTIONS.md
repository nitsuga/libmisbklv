---
type: Conventions
title: Knowledge bundle conventions
description: How the context/ OKF bundle is structured, written, and maintained.
tags: [meta, okf, conventions]
generated:
  by: claude/opus-5
  at: 2026-07-17T12:30:00Z
---

# Purpose

`context/` is the agent-facing knowledge base for libmisbklv, in
[OKF](https://github.com/GoogleCloudPlatform/knowledge-catalog/blob/main/okf/SPEC.md)
v0.2. It is the synthesis layer between immutable sources and the working
agent: knowledge is compiled here once and kept current, not re-derived each
session. The agent writes and maintains this directory; humans curate sources
and direct the work. On disagreement, the OKF spec wins over this doc.

# Layout

| Path | Role |
|------|------|
| `../references/` | Raw sources (standards PDFs + `.txt` extracts), append-only. Agent reads, and deposits new snapshots on a directed ingest; never edits one that's already there. |
| `context/` | This bundle. Agent-owned synthesis. |
| `../docs/` | Human-facing authored guides (terse). Not agent-maintained. |
| `../data/` | Ignored workspace for developer-provided media; not a test-data source. |
| `../test/fixtures/` | Project-owned deterministic KLV/MPEG-TS fixtures and generators. |
| `../planning/` | Project management (ROADMAP, PROGRESS). Transient; human + agent read/write. Not OKF concepts. |

The `.txt` next to each standards PDF is a faithful text extract for grep/Read.
Cite a standard by name and section (`ST 0601 §6.3`), not by extract line
number — line numbers shift across revisions.

# Frontmatter

Every concept `.md` carries YAML frontmatter. Required: `type` — the only key
OKF mandates. Recommended: `title`, `description`, `tags`, and `generated` (who
produced this, and when). Keep frontmatter minimal; OKF tolerates unknown keys
but they add noise.

```yaml
type: Standard Reference
title: Short display name
description: One-sentence summary.
tags: [st0601, fmv]
generated:
  by: claude/opus-5
  at: 2026-08-01T00:00:00Z
```

**Actors.** `generated.by` (and `verified[].by`) use the OKF actor convention:
`human:<id>` for people, `<producer>/<version>` for agents and tools (e.g.
`claude/opus-5`), `process:<id>` for automation. Trust tooling keys off the
`human:` prefix — use it only for genuine human authorship or review, never for
agent-written docs a human merely merged. Every doc in this bundle is
agent-written, so no `generated.by` here is ever `human:` — it names whichever
agent authored that doc (`claude/opus-5`, `openai/gpt-5`, …). More than one
model works this repo, so don't assume a single actor; the same spelling
identifies an agent posting to a PR, an issue, or a comment (`AGENTS.md`
§ Agent authorship on PRs, issues, and comments).

**`generated.at` is the last *meaningful* content change** — the spec's words,
and consumers use it to tell a recent edit from a stale fact. So bump it when
you change what the doc *claims*: a revised conclusion, a new section, a fact
updated against its source. Don't bump it for a typo, a reflow, or a link
repair — an `at` that tracks keystrokes tells a consumer nothing, and one that
never moves makes current knowledge look abandoned. `verified` is independent:
content can change without re-confirmation, and re-confirmation isn't a
content change.

Optional families — add one only when it earns its keep:

| Field | Use for |
|------|------|
| `resource` | Canonical URI/path of the asset the concept describes. Already in wide use here: the standards PDF a `Standard Reference` synthesizes, or the repo a `Prior Art` concept analyzes. |
| `sources` | Machine-readable inputs the concept derives from. Per entry `resource` is required; `id` when the body cites it; `title` optional. See § Citations and `sources`. |
| `verified` | List of `{by, at}` checks confirming the doc still matches its sources. |
| `status` | **OKF document lifecycle**: `draft` / `stable` (default) / `deprecated`. Not the ADR state — see § Decisions. |
| `stale_after` | `YYYY-MM-DD` after which the doc should be re-checked. |

**Migrating from OKF v0.1:** `timestamp` was replaced by `generated: {by, at}`,
and the body `# Citations` list by frontmatter `sources`. Consumers may fall
back to the legacy fields, so an un-migrated doc still reads — but write new
docs in the v0.2 spelling.

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

OKF v0.2 also defines one type with spec-level semantics, `Attested Computation`
(a concept carrying `runtime` / `parameters` / `computation` / `executor` /
`attester`, so a value's meaning and its verifiable derivation live together).
libmisbklv doesn't compute attested values — ignore this type here.

# Decisions

A **fork** is a decision point — a branch in the design or plan that needs a
choice (the metaphor is a fork in the road; it has nothing to do with a git fork).
Forks are the unit the roadmap and the register track.

A `type: Decision` concept is an ADR: the *why* of a resolved (or proposed)
fork — context, alternatives considered, the choice, consequences. Carry a
`decision_status:` frontmatter field:

| `decision_status` | Meaning |
|---|---|
| `proposed` | Under deliberation; not yet adopted. |
| `accepted` | Resolved and in force. |
| `superseded` | Replaced by a later Decision (link it). |
| `deferred` | Parked; revisit later. |

**Why not `status:`?** OKF v0.2 claims `status` for *document* lifecycle
(`draft` / `stable` / `deprecated`) — a different axis from *decision* state: a
superseded ADR is a stable document you keep for the record (e.g.
[`0009`](./decisions/0009-st0604-deferred.md) is `deferred`, but the document
itself isn't going anywhere). Since the spec wins on disagreement (§ Purpose),
the ADR field is `decision_status`, which OKF tolerates as an unknown key. An
ADR may still carry `status` in its OKF sense, but rarely needs to.

Lifecycle: an open fork starts under **Open forks** in
[`../planning/ROADMAP.md`](../planning/ROADMAP.md) (`OPEN`). Once deliberated,
write the Decision in [`./decisions/`](./decisions/index.md) with
`decision_status: proposed`; on resolution set `accepted`, add its row to the
**decided register** [`./decisions/index.md`](./decisions/index.md) (fork # ↔
ADR ↔ status), and refresh present state in
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
- **Frontmatter:** `type: Decision`, `title`, `decision_status`, `tags` (include
  `decision`, a topic tag, `phase-N`), `generated: {by, at}`, optional
  `sources:`, optional `fork:` (roadmap fork # for traceability).
- **Body:** `# Context` → `# Decision` → `# Alternatives considered` →
  `# Consequences` → `# Assumptions / open questions` → `# Citations`
  (specialized sections allowed between). Use `# Decision` always —
  `decision_status` carries proposed/accepted, so no header churn on accept.
- **Register:** [`./decisions/index.md`](./decisions/index.md) lists
  `Fork | ADR | Status`; updated on every status change.
- Decided in [`./decisions/0004-adr-format.md`](./decisions/0004-adr-format.md).

# Citations and `sources`

v0.2 replaces the body `# Citations` list with frontmatter `sources`. Here,
both have a job, because they carry different things:

- **`sources:` (frontmatter)** — the machine-readable index of *external*
  inputs: a standards PDF under `../references/`, a prior-art repo URL. One
  entry per input, `resource` required. This is what a consumer reads first.
- **`# Citations` (body)** — the annotated bibliography every concept and ADR
  in this bundle already writes: each entry says *why that source mattered to
  this doc*. Links to sibling concepts and ADRs
  (`[Title](./concept.md) — what it supplied`) live here.

Keep the annotation. A bare `resource:` list records that a source was
consulted; the prose records what it decided — and that reasoning is the
reason to keep an ADR (or a `Standard Reference`) at all. Purely internal
cross-references (other concepts, other ADRs) need no `sources` entry; the
body link is enough — most of this bundle's `# Citations` sections are exactly
that, and stay bare.

**Give every cited source an `id`.** The spec makes `id` optional but says it
SHOULD be present when the body cites the source — which here is every entry,
since `# Citations` annotates them all. The `id` is what makes attribution
machine-readable rather than a prose coincidence: without it, a consumer can see
*that* a doc has sources, but not which claim came from which one.

**Pin a specific claim with a footnote whose label is the `id`.** The label is
the join key into `sources`; a consumer resolves attribution through the
matching entry, not by reading the footnote text:

```markdown
---
sources:
  - id: st0903
    resource: ../references/ST0903.6.pdf
    title: MISB ST 0903.6 §9.2 — VMTI LS item/count limits
---

ST 0903 permits an unlimited number of VMTI LS items with no item size
limit.[^st0903]

[^st0903]: ST 0903.6 §9.2 — VMTI LS item/count limits.
```

Footnote the claims that would be *contested or checked* — a number, a quoted
rule, a constraint someone might dispute — not every sentence. Footnoting
everything restates the bibliography inline and is read as noise, which is how
attribution stops being read at all. A label with no matching `sources` entry
is a dangling citation: the spec doesn't say what a consumer should do with
one, so don't produce one (the link-check CI fails on it).

**Cite by stable anchor, not by position.** Name the section or clause
(`ST 0601 §6.3`, `ST 0903 §4.2`) — never a line or page number of the `.txt`
extract. Extracts get regenerated and revisions shift their numbering, so a
positional citation silently comes to point at the wrong text, which is worse
than a broken one: it still resolves, and it still looks right. The `.txt` file
next to each standards PDF is a reading aid for grep/Read, not a citable
address — always cite the standard itself.

The [`link-check`](../.github/workflows/link-check.yml) CI fails on the
mechanical form of this — a backticked `path.ext:NN` anywhere outside
[`log.md`](./log.md), whose dated entries are frozen snapshots. It cannot catch
the subtler case: a symbol anchor is only as stable as the symbol, so a function
that moves between files leaves a citation that still names it correctly and no
longer says where it is. That one stays a read-through item (§ Operations).

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
except `okf_version: "0.2"` at the bundle root. `log.md` — chronological change
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
