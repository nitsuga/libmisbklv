---
type: Decision
title: ST 0102 Security Metadata LS as a typed nested registry
decision_status: proposed
tags: [decision, registry, 0102, security, nested-ls, phase-3]
generated:
  by: claude/sonnet-5
  at: 2026-09-25T00:00:00Z
fork: 36
sources:
  - id: st0102
    resource: ../../references/ST0102.12.txt
    title: MISB ST 0102.12 §6.7 / Table 2 — Security Metadata Local Set Elements
---

# Context

Fork 36, opened from the UAS-platform coverage survey ([#95](https://github.com/nitsuga/libmisbklv/issues/95)). Tag 48 of the ST 0601 registry (`registry/uas0601.toml`) carries the ST 0102 Security Metadata Local Set today as `kind = "bytes"` — opaque passthrough, round-tripped faithfully but never decoded. `planning/PROGRESS.md` already named typing it "a candidate fork in ROADMAP"; `planning/ROADMAP.md` listed it as a candidate future fork alongside ST 1601/1602/1206, added in the same survey.

The codebase already has one precedent for a typed nested registry: ST 0903's `vTargetSeries` (tag 101) routes into `VTARGET_0903` via `kind = "pack"` + `child = "vtarget_0903"` (ADR 0010's `NestedLS`/`Pack` `ValueKind` variants, wired by the codegen in `tools/gen_registry.py`). ST 0102's Local Set is a single nested set, not a series of repeated packs, so it uses the sibling variant, `kind = "nested_ls"`, which the schema and codegen already support but no registry has exercised yet.

ST 0102.12 §6.7 / Table 2 defines the Local Set's 16-byte UL key (`06 0E 2B 34 02 03 01 01 0E 01 03 03 02 00 00 00`, CRC 40980) and 18 items (tags 1–14, 22–24; tags 15–21 are not defined). A Universal Set also exists (§6.6) but is out of scope here — nothing in this codebase decodes UDS-encoded metadata, and tag 48 only ever carries the Local Set form.

# Decision

**Add a new nested registry `SECURITY_0102`, wired the same way `VTARGET_0903` is, and change ST 0601 tag 48 from `bytes` to `nested_ls` pointing at it.** Concretely (for the follow-up implementation PR, not this one — this ADR decides the design, per the survey's own "decisions before implementation" ordering):

- `registry/security0102.toml`: `registry = "SECURITY_0102"`, `tag_encoding = "ber_oid"`, `ul_key = "060e2b34020301010e01030302000000"`.
- `registry/uas0601.toml` tag 48: `kind = "nested_ls"`, `child = "security_0102"`.
- `tools/gen_registry.py`: add `"security_0102": "Security0102"` to the `CHILD` dict.
- `include/misbklv/types.hpp`: add `Security0102` to `enum class RegistryId`.

Per-item typing, from ST 0102.12 Table 2:

| Tags | Kind | Length | Notes |
|---|---|---|---|
| 1 (Security Classification), 2 (country-coding method), 12 (object-country-coding method) | `uint`, 1 byte | fixed | Each is a byte enumeration (e.g. tag 1: UNCLASSIFIED=0x01 … TOP SECRET=0x05). No `ValueKind` here is a typed enum — ST 0601's own enumerated bytes aren't either — so the allowed-value table becomes a TOML comment, not a decode-time check. |
| 3 (Classifying Country), 4 (SCI/SHI), 5 (Caveats), 6 (Releasing Instructions), 7 (Classified By), 8 (Derived From), 9 (Classification Reason), 11 (Classification/Marking System), 14 (Classification Comments) | `utf8` | variable | Free-form or code-word text; ISO/IEC 646 is ASCII-compatible with UTF-8. |
| 10 (Declassification Date) | `utf8` | fixed, 8 | `YYYYMMDD` — a fixed-width string, not a numeric date type (no `ValueKind` in this schema decodes dates). |
| 13 (Object Country Codes) | `bytes` (**stays opaque**) | variable | RFC 2781 UTF-16 — see Alternatives; the schema has no UTF-16 `ValueKind` and one field isn't enough to justify adding one. |
| 22 (Version) | `uint`, 2 bytes | fixed | Standard's own version number, e.g. `0x000A` for ST 0102.10. |
| 23, 24 (coding-method version dates) | `utf8` | fixed, 10 | `YYYY-MM-DD`. |

Tags 1, 2, 3, 12, 13, 22 are "Required" per Table 2's own column and get `flags = ["mandatory"]`, enforced the same way ST 0601's mandatory items already are (`LocalSetBuilder::finish`, ADR 0011) — `enforce_mandatory` is a per-build parameter on any `Registry`, so building a Security LS through the same builder path checks *its own* mandatory items independently of the parent ST 0601 build. Tags 4–9, 11, 14, 23, 24 are "Context" or "Optional" and stay unflagged.

# Alternatives considered

- **Type tag 13 by adding a `Utf16` `ValueKind`.** Rejected for now: it touches the shared descriptor schema, the codegen's `KIND` dict, and every consumer that switches on `ValueKind` (codec decode/encode, any future pretty-printer) — a cross-cutting change to serve one field in one registry. If a second UTF-16 field turns up elsewhere, that's the point to add it; until then tag 13 stays `bytes`, which is strictly better than today (17 of 18 fields typed vs. 0) and loses nothing that isn't already lost.
- **Decode tag 13 by transcoding UTF-16 → UTF-8 in the codegen/codec, keeping `kind = "utf8"` for it.** Rejected: `Utf8` elsewhere in this schema means "the bytes are already UTF-8," not "decode-and-convert." Special-casing one item's *encoding rule* rather than its *type* would be a silent exception future maintainers would have to rediscover from the generated code, not the TOML.
- **A dedicated `enum` `ValueKind` for tags 1/2/12's byte enumerations.** Rejected: no existing registry has one despite ST 0601 having its own enumerated-byte items, so this would be a new pattern introduced for ST 0102 alone. `uint` plus a documentation comment matches the house style and costs nothing to add later if a real need appears (e.g. a pretty-printer wanting names).
- **Keep the whole LS opaque and only unwrap it at the application layer.** Rejected: that's the status quo the fork exists to change, and it's the reason ST 0102 fields (classification, releasability) don't currently show up in this library's own decode path at all.

# Consequences

- Tag 48 decodes are no longer opaque: 17 of 18 ST 0102 Local Set items become typed access, matching the ST 0601/ST 0903 pattern.
- Tag 13 (Object Country Codes) remains a `bytes` passthrough — round-trips correctly, but callers wanting object-country codes must still decode the UTF-16 payload themselves. Worth a one-line mention in `planning/PROGRESS.md` "Known gaps" once implemented, so it isn't mistaken for an oversight.
- A `LocalSetBuilder(security_0102_registry)` that omits tag 1, 2, 3, 12, 13, or 22 fails with `Error::MissingMandatory`, the same behavior ST 0601 mandatory items already have. Any code hand-assembling a Security LS payload (rather than treating it as opaque bytes) now needs to supply those six.
- No change to how ST 0601 itself is built or decoded except that tag 48's bytes are now interpreted rather than opaquely stored — existing callers that only round-trip messages (never inspecting tag 48's contents) see no behavior change.

# Assumptions / open questions

- **The Universal Set form (ST 0102 §6.6) is out of scope.** Nothing in this library encodes/decodes UDS Local Sets today, and tag 48 only ever carries the LS form in an ST 0601 stream. Revisit only if a future standard's nesting needs the UDS form.
- **Whether a real vendor stream's Security LS ever omits a "Required" item** (marking it "mandatory" would then reject valid real-world input on encode, though decode is unaffected — mandatory enforcement is a `LocalSetBuilder`/encode-side check per ADR 0011, not a decode-side rejection) is unconfirmed; ST 0102's own compliance language ("required security... information shall be contained") is what this ADR leans on. If real captures show otherwise, downgrading a flag is a one-line follow-up, not a re-fork.
- **Byte-enumeration values (tags 1, 2, 12) are recorded as TOML comments, not validated.** A value outside the standard's table decodes as whatever raw `uint` it is; nothing here rejects an unrecognized classification byte. That mirrors how ST 0601's own enumerated items already behave.

# Citations

[1] [ADR 0010](./0010-registry-descriptor-schema.md) — the `NestedLS`/`Pack` `ValueKind` variants and childRegistry routing this reuses.
[2] [ADR 0012](./0012-registry-codegen.md) — the TOML source format and codegen this extends with a new registry file.
[3] [ADR 0011](./0011-encode-model.md) — the mandatory-item enforcement model (`LocalSetBuilder::finish`) tags 1/2/3/12/13/22 opt into.
[4] `registry/vtarget0903.toml` / `registry/vmti0903.toml` — the one existing nested-registry precedent, `vTargetSeries` (tag 101), used as the concrete pattern to follow.
[5] ST 0102.12 §6.7, Table 2 — the Local Set item table this ADR's per-item typing decisions are read from.[^st0102]

[^st0102]: ST 0102.12 §6.7 / Table 2 — Security Metadata Local Set Elements: tag numbers, data types, and Required/Optional/Context column for every item.
