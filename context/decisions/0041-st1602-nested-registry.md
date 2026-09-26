---
type: Decision
title: ST 1602 Composite Imaging LS as a typed nested registry
decision_status: proposed
tags: [decision, registry, 1602, composite-imaging, nested-ls, phase-3]
generated:
  by: claude/sonnet-5
  at: 2026-09-26T00:00:00Z
fork: 37
sources:
  - id: st1602
    resource: ../../references/ST1602.2.txt
    title: MISB ST 1602.2 §7.2, Table 1 — Composite Imaging Local Set
---

# Context

Fork 37, split out of the UAS-platform coverage survey ([#95](https://github.com/nitsuga/libmisbklv/issues/95)) alongside ST 1601 and ST 1206. Tag 99 of the ST 0601 registry (`registry/uas0601.toml`) carries the ST 1602 Composite Imaging Local Set today as `kind = "bytes"` — opaque passthrough, never decoded.

This is split from ST 1601 into its own fork deliberately: ST 1601 (Geo-Registration) uses the MDARRAY pack type (ST 1303) for half its items and a raw UUID for another — real schema questions needing their own design conversation. ST 1602 has neither. All 18 of its items are `uint`, `int`, `uint8`, or `uint64` — types this codebase's registry schema already handles everywhere. Bundling the two would drag this one's trivial fork down to ST 1601's pace for no reason.

The precedent is the same one ST 0102 (fork 36, [ADR 0040](./0040-st0102-nested-registry.md)) used: ST 0601 tag 74 is `kind = "nested_ls"`, `child = "vmti_0903"` — a single nested Local Set, tested by `test/nested_roundtrip_test.cpp`. This ADR follows that pattern exactly.

ST 1602.2 §7.2 / Table 1 defines the Local Set's 16-byte UL key (`06 0E 2B 34 02 0B 01 01 0E 01 03 03 02 00 00 00`, CRC 666) and 18 items (tags 1-18, all defined, none skipped).

# Decision

**Add a new nested registry `COMPOSITE_IMAGING_1602`, wired the same way tag 74 → `VMTI_0903` and tag 48 → `SECURITY_0102` are, and change ST 0601 tag 99 from `bytes` to `nested_ls` pointing at it.** Concretely, for the follow-up implementation PR:

- `registry/compositeimaging1602.toml`: `registry = "COMPOSITE_IMAGING_1602"`, `tag_encoding = "ber_oid"`, no `ul_key` (nested-only, same reasoning as `vtarget0903.toml` and `security0102.toml` — a `ul_key` would make `Message::create()` constructible for a registry that's never meant to stand alone).
- `registry/uas0601.toml` tag 99: `kind = "nested_ls"`, `child = "composite_imaging_1602"`.
- `tools/gen_registry.py`: add `"composite_imaging_1602": "CompositeImaging1602"` to the `CHILD` dict.
- `include/misbklv/types.hpp`: add `CompositeImaging1602` to `enum class RegistryId`.
- `include/misbklv/registries.hpp`: add the generated-header include.
- `src/registries.cpp`: add a `case RegistryId::CompositeImaging1602:` arm to `registry_for()`.
- `CMakeLists.txt`'s `regenerate-registry` target and `.github/workflows/ci.yml`'s drift-check loop: add `compositeimaging1602`.

Per-item typing, from ST 1602.2 Table 1:

| Tags | Kind | Length | Notes |
|---|---|---|---|
| 1 (Precision Time Stamp) | `uint`, 8 bytes | fixed | Matches how Precision Time Stamp is typed everywhere else in this codebase (ST 0601 tag 2, ST 0903 tag 2) — same field, same encoding. Optional (may be omitted if identical to the parent's). |
| 2 (Document Version) | `uint`, 1 byte | variable | Mandatory (ST 1602-03). Table 1's Type column is plain `uint` — same "application defines the width" treatment as tags 3-16, not a fixed-width field. 1 byte is a generous default (decode accepts whatever width the encoder actually used, 1-8 bytes, per `codec::decode`); it isn't itself a precedent, just a sensible default for a value that will realistically never exceed 255 for a `.2`-revision standard. |
| 3-16 (Source/AOI/Sub-Image/Active-Sub-Image rows, columns, positions, offsets) | `uint` (tags 3, 4, 5, 6, 9, 10, 13, 14) or `int` (tags 7, 8, 11, 12, 15, 16), 4 bytes | variable | The `uint`/`int` split is exactly Table 1's own Type column, not inferred from range or sign behavior — e.g. tags 15/16 (Active Sub-Image Offset X/Y) are `int` in Table 1 despite the standard itself saying their valid range is "values greater than zero" (§7.3.5.7/.8); `signed = true` on the `int` items follows the same TOML shape as `uas0601.toml`'s own variable-length signed ints (tags 136, 137). ST 1602 doesn't specify a byte width for any of these — "the application defines the number of bytes needed" (§7.2) — so 4 bytes is a new choice for this registry, not an existing house convention: ADR 0029's rule ("default width = the standard's own maximum") doesn't apply here since ST 1602 states no maximum. The choice is low-risk regardless, since `codec::decode` accepts any width 1-8 bytes a real encoder used. Tags 9-12 (Sub-Image Rows/Columns/Position X/Y) are Mandatory; the rest are Optional. |
| 17 (Transparency) | `uint`, 1 byte | fixed | 0-255 range fits exactly; Optional, defaults to 0 (opaque) if absent per the standard — worth a TOML comment, not enforced. |
| 18 (Z-Order) | `uint`, 1 byte | fixed | Mandatory. The standard also requires values be unique and non-zero across composited images (ST 1602-04) — a cross-item, cross-message constraint this registry cannot express or enforce; recorded as a comment. |

**Mandatory flags: tags 2, 9, 10, 11, 12, 18** get `flags = ["mandatory"]`, matching Table 1's Rules column exactly. Unlike ST 0102, there's no checksum-tag collision here: `check_mandatory()` (`src/builder.cpp`) doesn't special-case tag 1 at all — it just iterates `reg_.items` for the `mandatory` flag — and `finalize()` (the only place that *does* treat tag 1 specially, for the checksum) is never reachable on a registry with no `ul_key`. ST 1602's own tag 1 (Precision Time Stamp) is an ordinary, Optional data field with no interaction with either path. So `LocalSetBuilder::check_mandatory()` (added in ADR 0040, already merged) applies here with no new builder work — a caller authoring a Composite Imaging LS calls `check_mandatory()` before `serialize_items()`, exactly as ST 0102 does.

# Alternatives considered

- **One combined ADR/fork for ST 1601 and ST 1602.** Rejected — see Context. They're unrelated standards sharing nothing but "currently opaque under ST 0601"; ST 1602 has no open design questions and ST 1601 has real ones (MDARRAY, UUID). Forcing them together would either stall the easy one or under-deliberate the hard one.
- **Fixed byte widths other than 4 for tags 3-16.** Considered 2 bytes (covers up to 65535, likely suffices for real image dimensions) but rejected in favor of 4 bytes as a more generous, still-cheap default — this isn't an existing house convention (see the per-item table above; ADR 0029's actual rule doesn't apply when the standard states no maximum), just a reasonable choice, made low-risk by `codec::decode` accepting whatever width a real encoder used regardless of what this registry declares as default.
- **Enforcing the Z-Order uniqueness/non-zero constraint (ST 1602-04) at encode time.** Rejected: it's a cross-item (Z-Order != 0) and cross-message (unique among sibling Composite Imaging LSs in the same stream) constraint. `check_mandatory()` validates presence of a single registry's own tags; it has no concept of sibling messages or a mandatory range/uniqueness check. Building that would be new machinery for a rule this registry's typing doesn't need to enforce to be useful — a caller wanting the real requirement checked does it in application code, same as any other cross-field business rule this library doesn't police (e.g. ST 0601's own many range constraints beyond what special values catch).
- **Keep the whole LS opaque and only unwrap it at the application layer.** Rejected: that's the status quo the fork exists to change.

# Consequences

- Item descriptors for tag 99's contents become available for caller-driven descent, the same pattern already exercised for VMTI (tag 74) and Security0102 (tag 48) — `Message::get(99)` itself is unchanged and still returns raw bytes; the library does not auto-decode nested sets.
- **Reaching tag 99 in practice usually means decoding tag 100 (the Segment LS, ST 1607) by hand first.** ST 1602's own scope text places the Composite Imaging LS inside a Segment LS, which is still `kind = "bytes"` in `uas0601.toml` (untyped, out of scope here) — so a caller descends into tag 100's raw value, finds tag 99 within it, and only then reaches these new descriptors. This ADR doesn't change that; it's the same "typed once you get there" property ST 0102 already has for callers who reach it via a similarly opaque wrapper.
- A caller authoring a Composite Imaging LS who calls `check_mandatory()` before nesting it and omits tag 2, 9, 10, 11, 12, or 18 gets `Error::MissingMandatory`. No new builder code is needed — `check_mandatory()` already exists from ADR 0040.
- CI's generated-output drift check and the `regenerate-registry` build target must both learn about `compositeimaging1602.toml`, or the new file silently never regenerates.
- No change to how ST 0601 itself is decoded except that tag 99's descriptors are now resolvable.

# Assumptions / open questions

- **Byte widths for tags 3-16 are this registry's own choice, not the standard's.** ST 1602 explicitly leaves them to the implementer. 4 bytes is generous for any realistic image dimension or offset; if a real stream is ever seen using something ST 1602 disallows outright (it doesn't — the standard just says "application-defined"), this is a one-line TOML change, not a re-fork.
- **The Z-Order uniqueness/non-zero rule (ST 1602-04) is unenforced**, per Alternatives. Decode is unaffected either way; only an authoring caller who wants that guarantee needs to implement it themselves.
- **Whether real encoders always include Optional items like Transparency (defaulting to 0 when absent) or omit them is unconfirmed** — doesn't affect typing, only worth knowing if a future consumer needs a get-with-default helper (not proposed here).

# Citations

[1] [ADR 0010](./0010-registry-descriptor-schema.md) — the `NestedLS` `ValueKind` and childRegistry routing this reuses.
[2] [ADR 0012](./0012-registry-codegen.md) — the TOML source format and codegen this extends with a new registry file.
[3] [ADR 0040](./0040-st0102-nested-registry.md) — the direct precedent this ADR follows (nested-only registry, no `ul_key`, `check_mandatory()` for authoring), and the source of `check_mandatory()` itself, reused here with no changes.
[4] `registry/uas0601.toml` tag 74 (`kind = "nested_ls"`, `child = "vmti_0903"`) and tag 48 (`kind = "nested_ls"`, `child = "security_0102"`) — the two existing precedents for this fork's shape.
[5] ST 1602.2 §7.2, Table 1 — the Local Set item table this ADR's per-item typing decisions are read from.[^st1602]

[^st1602]: ST 1602.2 §7.2 / Table 1 — Composite Imaging Local Set: tag numbers, units, data types, and Mandatory/Optional column for every item.
