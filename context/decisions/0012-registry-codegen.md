---
type: Decision
title: Registry codegen — source format & build integration
decision_status: accepted
tags: [decision, architecture, registry, codegen, build, phase-3]
generated:
  by: claude/opus-5
  at: 2026-07-17T22:30:00Z
fork: 10
---

# Context

Fork 10 — the follow-on ADR [`0006`](./0006-tag-registry.md) left open: the
descriptor **source-of-truth format** and the **codegen tool**.
[`0006`](./0006-tag-registry.md) fixed *compiled-in `constexpr`*;
[`0010`](./0010-registry-descriptor-schema.md) fixed the *schema* (the
`ItemDescriptor` / `Registry` shape codegen must emit). This ADR fixes how the
~143-item 0601 table (plus 0903 and its embedded LSs) is authored and turned
into those `constexpr` tables.

The input data (a) changes rarely — only when MISB revs a standard or we add
items; (b) is tabular with per-item nested structure (special-value lists,
mapping params); (c) benefits from citing the standard section per row.

# Decision

**TOML source-of-truth + a Python generator; generated C++ is committed, with a
regen target and a CI drift check.**

## Source format: TOML

One file per registry (`registry/0601.toml`, `registry/0903.toml`, plus the
embedded LSs). Array-of-tables maps cleanly to the schema, comments carry the
per-item standard citation, and diffs stay line-oriented for review. Sketch:

```toml
registry     = "UAS_0601"
tag_encoding = "ber_oid"
ul_key       = "060e2b34020b01010e0103010100000000"

[[item]]
tag = 2
name = "Precision Time Stamp"
units = "microseconds"
kind = "uint"           # ValueKind
length = 8              # fixed
flags = ["mandatory"]

[[item]]                # 0601.19 §8.13
tag = 13
name = "Sensor Latitude"
units = "deg"
kind = "linear_lds"
length = 4
signed = true
min = -90.0
max = 90.0
  [[item.special]]
  pattern = "0x80000000"
  kind = "reserved"
```

## Generator: Python (`tools/gen_registry.py`)

Reads the TOML (stdlib `tomllib`, Python ≥ 3.11), **validates**, and emits the
`constexpr` C++ per [`0010`](./0010-registry-descriptor-schema.md). Validation
(fail the build on violation): tag uniqueness within a registry; mapping
params present iff `kind ∈ {linear_lds, imapb}`; `childRegistry` resolvable for
`nested_ls`/`pack`; special-value `pattern` fits `length`; sorted-by-tag output
for binary search. Python is already in use (the spike); it is a *host*-side
tool, unaffected by aarch64 cross-compile.

## Build integration: commit generated code + regen target + drift check

The generated header(s) (`include/misbklv/registry/*.generated.hpp`) are
**checked in**, not build-time artifacts. Rationale:

- **No Python dependency for normal builds** — the library's registry code and
  project-owned test fixtures are committed outputs (matters for the
  embedded/Jetson/closed-app targets [`0006`](./0006-tag-registry.md) optimizes
  for). Python stays an explicit contributor-side regeneration tool.
- The tables are **in-tree, greppable, reviewable** — a code review sees the
  actual constants.
- Regeneration is explicit: a CMake target (`regenerate-registry`) runs the
  generator; **CI runs it and fails if the tree changes** (`git diff --exit-code`),
  guaranteeing source and generated stay in sync.

# Alternatives considered

- **JSON source** — ubiquitous but no comments (can't cite the standard inline),
  verbose, less pleasant to hand-edit. Better as a *machine* interchange than a
  hand-authored source. Rejected.
- **YAML source** — human-friendly but whitespace-fragile and needs a non-stdlib
  parser (`pyyaml`). Rejected for the stdlib-`tomllib` win + robustness.
- **Directly-authored `constexpr` C++** (no codegen) — simplest build, but
  editing 143 rows of struct-initializers is error-prone, not independently
  validatable, and couples the data to the language ([`0006`](./0006-tag-registry.md)
  rejected this as "not data-driven"). Rejected.
- **CMake-only generation** — CMake string processing is painful for structured
  transforms + validation. Rejected.
- **Build-time generation** (`add_custom_command`, generated header not
  committed) — cleaner "single source" story, but makes Python a hard build
  dependency for every consumer and hides the tables from review. Rejected for
  v1 in favour of checked-in + drift check; revisit if the data starts changing
  often enough that stale-regen friction outweighs the dependency cost.

# Consequences

- New tree: `registry/*.toml` (source), `tools/gen_registry.py` (generator),
  `include/misbklv/registry/*.generated.hpp` (committed output).
- CMake gains a `regenerate-registry` target (not in the default build); CI
  gains a drift-check step (`regenerate` → `git diff --exit-code`).
- Adding an item = edit TOML + `regenerate-registry` + commit; adding a standard
  = new TOML + a `RegistryId` enum entry.
- Installed-library consumers and normal source checkouts build with **no
  Python**; contributors regenerating registries or synthetic fixtures use the
  same Python 3.11+ host-side toolchain.
- **Coverage lives in the TOML, so it is reviewable in one place.**
  `registry/0601.toml` carries every §8 item of ST 0601.19 except the deprecated
  Item 66 — so the sample streams decode with no unregistered tags. Items the
  standard defines as another standard's Local Set, or as a DLP/FLP/VLP pack, are
  registered as named opaque `bytes`: addressable and byte-identical on
  round-trip, without a typed nested registry. `registry/0903.toml` is filled as
  data needs it rather than exhaustively.
- The generator is the natural place to later *scrape* the standard's item table
  (`ST0601.19.txt` ~line 658) into TOML, if we automate ingestion.

# Assumptions / open questions

- Python ≥ 3.11 for `tomllib` during explicit registry or fixture
  regeneration. If an older Python must regenerate, add `tomli`. Non-blocking.
- Exact generated-file layout (one header vs per-registry) — settle in
  implementation; a single `.generated.hpp` is the default.
- Whether to also emit a small runtime self-check (table sorted / non-empty) —
  optional; the generator already validates at author time.

# Citations

[1] [`0006`](./0006-tag-registry.md) — compiled-in `constexpr`; left source
    format + codegen tool open (this ADR closes them).
[2] [`0010`](./0010-registry-descriptor-schema.md) — the `ItemDescriptor` /
    `Registry` shape the generator emits.
[3] [`0001`](./0001-build-system-and-cpp-standard.md) — CMake ≥ 3.20; aarch64
    cross-compile (generator is host-side, unaffected).
[4] [`st0601`](../st0601.md) — item table (`ST0601.19.txt` ~line 658) as the
    eventual TOML population source.
