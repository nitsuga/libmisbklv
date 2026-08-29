---
type: Decision
title: ADR 0037 — Share a two-slot capacity gate across self-hosted CI
description: Regular libmisbklv and parrot-to-klv CI jobs share two host capacity slots, allowing two parrot jobs in parallel while bounding combined self-hosted workload.
decision_status: accepted
tags: [decision, ci, security, self-hosted, parallelism]
generated:
  by: openai/gpt-5
  at: 2026-08-29T15:15:34Z
sources:
  - id: libmisbklv-67
    resource: https://github.com/nitsuga/libmisbklv/issues/67
    title: Share a two-slot capacity gate with parrot-to-klv
  - id: parrot-to-klv-108
    resource: https://github.com/nitsuga/parrot-to-klv/issues/108
    title: Run build and sanitizer jobs in parallel on self-hosted runners
  - id: github-workflow-syntax
    resource: https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#concurrency
    title: GitHub Actions workflow syntax — concurrency
---

# Context

PR 66 moved libmisbklv's regular CI to three repository-scoped self-hosted
runners on a seven-CPU host. Parrot-to-klv has one runner on that same host and
needs a second so its build and sanitizer jobs can run concurrently. The
repositories may overlap occasionally; manual timing is not a safety control.

GitHub Actions concurrency groups are scoped to a repository, so matching group
names in the two repositories cannot protect this shared host. Each normal
compile is capped at three build processes. The host therefore needs a small
shared capacity bound rather than repository-local workflow concurrency.

# Decision

Add the same small `ci/with-capacity.sh` helper to both repositories. It uses
two `flock` files in the existing `/home/eric/ci-cache` host mount. Normal
build-test, sanitizer, generated-drift, and parrot commands claim one slot;
the CI image publisher claims both because it uses the host Docker daemon.
Hosted fork jobs use an ephemeral `/tmp` lock path. Trusted parrot jobs use two
repository-scoped self-hosted runner registrations labeled `parrot-to-klv`.

# Alternatives considered

- **Rely on manual non-overlap** — rejected because “rare” is not an enforced
  safety property.
- **Use GitHub concurrency groups** — rejected because the groups are scoped to
  a repository, not the shared host.
- **Keep one parrot runner** — rejected because build and sanitizer checks remain
  serialized.
- **Use a separate host** — rejected as unavailable.
- **Put all jobs behind one global lock** — rejected because it would remove the
  desired parallelism. Two slot locks preserve it while bounding load.

# Consequences

Two parrot jobs can run concurrently, and occasional overlap with parrot-to-klv
is bounded to two normal jobs at `-j3`. Jobs may occupy a runner while waiting
for a slot. The lock files depend on the existing shared host mount, and both
repositories must keep the helper and slot count aligned.

# Assumptions / open questions

The host continues to provide seven CPUs and normal builds continue to use
`-j3`. If either changes, the slot count or per-job build parallelism must be
re-evaluated. The gate is a capacity bound, not a cross-repository workflow
queue or fairness guarantee.

# Citations

- [GitHub Actions workflow syntax — concurrency](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#concurrency) — documents that concurrency groups operate within the repository.
- [libmisbklv #67](https://github.com/nitsuga/libmisbklv/issues/67) — records the shared-host capacity requirement.
- [parrot-to-klv #108](https://github.com/nitsuga/parrot-to-klv/issues/108) — records the parrot parallelism requirement.
