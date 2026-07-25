---
type: Conventions
title: Why these rules
description: The failure mode behind each planning-hygiene rule in CLAUDE.md.
tags: [meta, conventions, hygiene]
timestamp: 2026-07-25T00:00:00Z
---

# Why these rules

The hygiene rules in [`../CLAUDE.md`](../CLAUDE.md) § Planning hygiene look fussy
until you've watched docs rot. **Read this before relaxing one** — each earned its
place by a failure mode, most of them observed in this repo:

- **One job per doc.** When two docs narrate the same thing, one copy drifts and
  you can't tell which is right. So: history lives only in [`log.md`](./log.md),
  the decided register only in [`decisions/index.md`](./decisions/index.md),
  present state only in [`PROGRESS.md`](../planning/PROGRESS.md).
- **History → `log.md`, present-state → `PROGRESS.md`.** A curated knowledge base
  must not carry volatile session history, and a status doc must not accrete a
  "Done" pile. Keeping them separate keeps each trustworthy.
- **Docs land in the *same commit* as the code.** A doc updated "later" is a doc
  that is wrong in between — and agents, reviewers, and your future self all read
  the repo *at a commit*, not at "later". Deferring also means reconstructing what
  changed from memory, so the entry arrives thin or never arrives at all. The
  checklist at the top of `CLAUDE.md` is the gate that enforces it.
- **No live tallies.** A count ("19 tests", "forks 1–15") is wrong the moment the
  next change lands, and nobody remembers to update prose numbers. Describe the
  *state* ("all CTest cases green", "none open — see the register"); a number is
  only ever a frozen snapshot in a dated `log.md` line.
- **Closing scrubs the future / one home for open work.** Forward-looking content
  drifts exactly like history: resolve an item but leave it listed under "Next" /
  "candidate" elsewhere, and the docs now lie. This one has a concrete origin —
  ROADMAP and PROGRESS both carried candidate lists, so resolved forks (the
  stop-token, multi-byte BER-OID tags) survived in the copy nobody was editing.
  So a candidate lives in *one* place (ROADMAP's backlog), and resolving it means
  deleting its forward-looking mentions too — not just updating the present-state
  bullet.
- **Lint is the backstop.** Nothing fires it automatically, so run it periodically
  (closing a fork, before a release) — see [`CONVENTIONS.md`](./CONVENTIONS.md)
  § Operations. But the real lever is reducing what *can* drift (the rules above),
  so the lint has little to catch.

The meta-lesson: **duplication rots — in both tenses.** Most of these rules are
one idea (don't say the same thing in two places) applied to the past (history,
decisions) and the future (open/candidate work).

This file is the single home for that rationale. It lives in `context/` — not the
top-level `README.md` — so the project's README stays human-facing: what
libmisbklv is, how to build it, how to use it. See
[`CONVENTIONS.md`](./CONVENTIONS.md) § Layout for the directory taxonomy.
