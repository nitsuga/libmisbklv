---
type: Decision
title: Tag registry — compiled-in
decision_status: accepted
tags: [decision, architecture, registry, phase-2]
generated:
  by: claude/opus-5
  at: 2026-07-17T19:30:00Z
fork: 4
---

# Context

Fork 4 (split). Per-standard item descriptors (0601's ~143 items, 0903's) must
be data-driven, not hand-written classes (prior art:
[`gstklvplugin`](../prior-art-gstklvplugin.md)'s INI;
[`klvp`](../prior-art-klvp.md)'s `ldsdp`). Question: runtime data file vs
compiled-in.

# Decision

**Compiled-in `constexpr` tables, generated from a source-of-truth at build
time.** Item descriptors live in a source-of-truth (data file) that a
build-time generator emits as `constexpr` C++ tables baked into the library.
Adding an item = edit the source, regenerate, recompile — no hand-written class,
no runtime file dependency.

Rationale: no runtime load (embedding / Jetson / closed-app friendly); faster
startup (no parse); single artifact; still data-driven (source-of-truth, not
code-per-item).

# Alternatives considered

- **Runtime data file** (INI/TOML, gstklvplugin-style) — runtime-editable but
  adds a file dependency + startup parse; embedding-unfriendly. Rejected for v1
  (revisit if hot-editability is ever required).
- **Hand-coded C++ per item** (klvdata-style) — not data-driven. Rejected.
- **Compiled-in (chosen)** — data-driven with no runtime dependency.

# Consequences

- Build gains a codegen step (CMake `add_custom_command`). The registry is a
  compile-time constant (fast lookup, no runtime alloc).
- Adding items / standards = edit source + regenerate. A future
  runtime-loaded registry remains possible if needed.

# Assumptions / open questions

- Source-of-truth format: a data file (TOML/JSON/YAML) + generator vs a
  directly-authored `constexpr` C++ table. Lean: data file + generator
  (editable without C++ fluency, validatable, shareable). Settle in
  implementation.
- Codegen language (Python script vs CMake) — TBD.

# Citations

[1] [`gstklvplugin`](../prior-art-gstklvplugin.md) — INI pattern (rejected as
    runtime).
[2] [`klvp`](../prior-art-klvp.md) — `ldsdp` DB.
[3] [`0601`](../st0601.md) Table 1 — the item source.
