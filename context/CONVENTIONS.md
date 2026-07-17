---
type: Conventions
title: Knowledge bundle conventions
description: How the context/ OKF bundle is structured, written, and maintained.
tags: [meta, okf, conventions]
timestamp: 2026-07-17T12:30:00Z
---

# Purpose

`context/` is the agent-facing knowledge base for libklv, in
[OKF](https://github.com/GoogleCloudPlatform/knowledge-catalog/blob/main/okf/SPEC.md)
v0.1. It is the synthesis layer between immutable sources and the working
agent: knowledge is compiled here once and kept current, not re-derived each
session. The agent writes and maintains this directory; humans curate sources
and direct the work. On disagreement, the OKF spec wins over this doc.

# Layout

| Path | Role |
|------|------|
| `../references/` | Immutable raw sources (standards PDFs + `.txt` extracts). Agent reads, never modifies. |
| `context/` | This bundle. Agent-owned synthesis. |
| `../docs/` | Human-facing authored guides (terse). Not agent-maintained. |
| `../data/` | Sample MPEG-TS test vectors. |

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
| `Component` | A libklv module (KLV core, gstreamer backend, ffmpeg backend). |

Anticipated but not yet used: `Sample Data`, `Test Vector`. Add as needed.

# Linking

Prefer bundle-relative absolute links (begin with `/`, resolved relative to
`context/`): `[ST 0601](/st0601.md)`. Relationship semantics — references,
nests-in, depends-on — live in the surrounding prose; the link itself is
untyped. Broken links are not errors; they may be not-yet-written knowledge.

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
superseded by newer sources, orphan concepts with no inbound links, concepts
mentioned but not written, broken-link targets worth creating, and type drift
(synonyms / casing). Tidy the `type` list when synonyms appear.

# Source discipline

Never edit files in `../references/`. The immutable PDFs are the source of
truth; this bundle is the curated, cross-linked reading of them.
