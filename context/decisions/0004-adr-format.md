---
type: Decision
title: ADR format & numbering
status: accepted
tags: [decision, adr, conventions, phase-1]
timestamp: 2026-07-17T18:30:00Z
fork: 7
---

# Context

Fork 7 of [`../../planning/ROADMAP.md`](../../planning/ROADMAP.md). Three ADRs
are already in flight — [`0001`](./0001-build-system-and-cpp-standard.md)
(build), [`0002`](./0002-license.md) (license), [`0003`](./0003-project-name.md)
(name) — using a de-facto format. Fork 7 formalizes it before more accumulate.
Location is already settled: `context/decisions/` with a register
(`index.md`). The full spec lives in
[`../CONVENTIONS.md`](../CONVENTIONS.md) § Decisions → ADR format; this ADR is
the *decision*, CONVENTIONS is the *spec*.

# Decision

Adopt the **Nygard-style** ADR format already in de-facto use:

- **Filename:** `NNNN-slug.md` — 4-digit zero-padded, hyphenated lowercase slug
  (this file is the example).
- **Numbering:** sequential by **creation order**, not roadmap-fork number (they
  diverge: fork 7 → ADR 0004). Monotonic; never reused. A superseded ADR keeps
  its number; the superseding ADR takes the next.
- **Frontmatter:** `type: Decision`; `title`; `status`
  (proposed|accepted|superseded|deferred); `tags` (include `decision`, a topic
  tag, `phase-N`); `timestamp` (ISO 8601, last meaningful change); optional
  `fork:` (roadmap fork # for traceability).
- **Body:** `# Context` → `# Decision` → `# Alternatives considered` →
  `# Consequences` → `# Assumptions / open questions` → `# Citations`
  (specialized sections allowed between). Use `# Decision` always — `status`
  carries proposed/accepted, so no header churn on accept.
- **Register:** `context/decisions/index.md` lists `Title | Status` by number;
  updated on every status change.

# Alternatives considered

- **MADR** (Markdown ADR) — more structured (fixed options/fields); more
  overhead than our lightweight lib needs. Rejected.
- **No convention (ad hoc)** — what we had before fork 7; inconsistent as the
  count grows. Rejected now that we have 3+ ADRs.
- **External-tool / schema-enforced formats** — overkill; we want plain
  `cat`-readable markdown with no tooling dependency. Rejected.

# Consequences

- All future ADRs follow `NNNN-slug.md` + the frontmatter/body above.
- ADR numbers diverge from fork numbers (fork 7 = ADR 0004); the register and
  ROADMAP's Decision-link column reconcile this.
- Status transitions (proposed → accepted → superseded) are recorded in the
  `status` field + the bundle `log.md`; no separate per-ADR changelog (`timestamp`
  = last change suffices for now).

# Assumptions / open questions

- **Resolved by precedent (0001–0003):** filename, numbering, frontmatter,
  body sections, register.
- **Non-blocking:** per-ADR status-change dates — `timestamp` + `log.md` suffice;
  add a status-history block only if supersession frequency warrants.

# Citations

[1] M. Nygard, "Documenting Architecture Decisions" — the `NNNN-slug` /
    Context-Decision-Consequences pattern.
[2] [`../CONVENTIONS.md`](../CONVENTIONS.md) § Decisions → ADR format — the
    spec home.
[3] Existing examples: [`0001`](./0001-build-system-and-cpp-standard.md),
    [`0002`](./0002-license.md), [`0003`](./0003-project-name.md).
