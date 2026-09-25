---
type: Decision
title: ST 0102 Security Metadata LS as a typed nested registry
decision_status: accepted
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

The codebase already has a direct precedent for exactly this shape: ST 0601 tag 74 (`registry/uas0601.toml`) is `kind = "nested_ls"`, `child = "vmti_0903"` — a single nested Local Set, tested by `test/nested_roundtrip_test.cpp`. (ST 0903's own `vTargetSeries`, tag 101, is the sibling `pack` variant — a *series* of repeated child packs, which ST 0102 does not need.) This ADR follows tag 74's pattern exactly: same `ValueKind::NestedLS`, same one-item-one-child shape.

ST 0102.12 §6.7 / Table 2 defines the Local Set's 16-byte UL key (`06 0E 2B 34 02 03 01 01 0E 01 03 03 02 00 00 00`, CRC 40980) and 17 items (tags 1–14, 22–24; tags 15–21 are undefined in this revision — likely where the deprecated linking elements, Appendix A, used to sit in earlier versions). A Universal Set also exists (§6.6) but is out of scope: ST 0601.14-31 (`references/ST0601.19.txt`) confirms tag 48 always carries the LS form, never the UDS form.

**What "typed" means here, precisely.** `.child` is resolved only through `registry_for()` (`src/registries.cpp`) when a caller chooses to descend — the library never auto-descends into a nested set on decode. `Message::get(48)` still returns the raw bytes; a caller who wants Security LS fields looks up the tag-48 descriptor, calls `registry_for(d->child)` to get the `Security0102` registry, `parse_items()`s the raw value into child items, then looks each up via `child->find(tag)` and `codec::decode()` — the same walk `test/nested_roundtrip_test.cpp` does for VMTI today, with no top-level `Message` involved (there is no `ul_key`, deliberately — see Decision). This ADR makes that path exist for ST 0102 tag 48; it does not change what `Message::get(48)` returns.

# Decision

**Add a new nested registry `SECURITY_0102`, wired the same way tag 74 → `VMTI_0903` is, and change ST 0601 tag 48 from `bytes` to `nested_ls` pointing at it.** Concretely, for the follow-up implementation PR (this ADR decides the design; issue #95 explicitly orders decisions before implementation):

- `registry/security0102.toml`: `registry = "SECURITY_0102"`, `tag_encoding = "ber_oid"`, **no `ul_key`** — see Alternatives; this registry is nested-only, the same as `vtarget0903.toml`'s "no standalone UL key."
- `registry/uas0601.toml` tag 48: `kind = "nested_ls"`, `child = "security_0102"`.
- `tools/gen_registry.py`: add `"security_0102": "Security0102"` to the `CHILD` dict.
- `include/misbklv/types.hpp`: add `Security0102` to `enum class RegistryId`.
- `include/misbklv/registries.hpp`: add `#include "misbklv/registry/security0102_tables.generated.hpp"` — it explicitly includes every generated registry header, and `src/registries.cpp` can't reference `gen::security_0102` without it.
- `src/registries.cpp`: add a `case RegistryId::Security0102:` arm to `registry_for()` — omitting this leaves `.child` resolving to `nullptr`, silently breaking descent.
- `CMakeLists.txt`'s `regenerate-registry` target and `.github/workflows/ci.yml`'s drift-check loop (`for r in uas0601 vmti0903 vtarget0903`): both enumerate registry files by name and need `security0102` added, or the new TOML never regenerates and CI's generated-output drift check never covers it.
- `include/misbklv/builder.hpp` / `src/builder.cpp`: add `Result<std::monostate> check_mandatory() const;` to `LocalSetBuilder` — the loop `finalize()` already runs (`for (const auto& d : reg_.items) if ((d.flags & kMandatory) && !staged_contains(d.tag)) return Error::MissingMandatory`), pulled out so it can run without emitting a checksum. `finalize()`'s `enforce_mandatory` branch becomes `if (auto r = check_mandatory(); !r) return Result<ber::Bytes>::err(r.error());` — same check, same error, adapted to `finalize`'s own `Result<ber::Bytes>` return type. See Decision below for why this exists and how it's used.
- `test/nested_roundtrip_test.cpp` (or a small addition near it): a test building a `Security0102` LS via `LocalSetBuilder` that (a) omits tag 22 and asserts `check_mandatory()` returns `Error::MissingMandatory`, and (b) stages all six required tags and asserts it returns ok. Without this, `check_mandatory()` ships with no caller anywhere in the tree — `serialize_items()` itself has no production caller today either (only `rebuild_items` test helpers, which deliberately don't enforce), so this method needs its own direct coverage rather than relying on `finalize()`'s existing tests.

Per-item typing, from ST 0102.12 Table 2:

| Tags | Kind | Length | Notes |
|---|---|---|---|
| 1 (Security Classification), 2 (country-coding method), 12 (object-country-coding method) | `uint`, 1 byte | fixed | Each is a byte enumeration (e.g. tag 1: UNCLASSIFIED=0x01 … TOP SECRET=0x05). No `ValueKind` here is a typed enum — ST 0601's own enumerated bytes aren't either — so the allowed-value table becomes a TOML comment, not a decode-time check. |
| 3 (Classifying Country), 4 (SCI/SHI), 5 (Caveats), 6 (Releasing Instructions), 7 (Classified By), 8 (Derived From), 9 (Classification Reason), 11 (Classification/Marking System), 14 (Classification Comments) | `utf8` | variable | Free-form or code-word text. |
| 10 (Declassification Date) | `utf8` | variable | `YYYYMMDD`, always 8 bytes per the standard — but no `utf8` item elsewhere in this codebase carries a `length`, and neither the codec nor the codegen enforces one on `Utf8` (`FIXED_KINDS` in `tools/gen_registry.py` excludes it). Record "8 bytes, YYYYMMDD" as a TOML comment, the same treatment as the tag 1/2/12 enum tables, rather than a `length` field that would silently do nothing. |
| 13 (Object Country Codes) | `bytes` (**stays opaque**) | variable | RFC 2781 UTF-16 (big-endian absent a BOM) — see Alternatives; the schema has no UTF-16 `ValueKind` and one field isn't enough to justify adding one. |
| 22 (Version) | `uint`, 2 bytes | fixed | Standard's own version number, e.g. `0x000A` for ST 0102.10. Absent, ST 0102 §6.1.15/-57 says version 3 is assumed — worth a comment, not enforcement. |
| 23, 24 (coding-method version dates) | `utf8` | variable | `YYYY-MM-DD`, always 10 bytes — same "comment, not `length`" treatment as tag 10. |

**Mandatory flags: added, enforced by a new checksum-free check.** Table 2 marks tags 1, 2, 3, 12, 13, 22 "Required." The obvious move — `flags = ["mandatory"]` plus the existing enforcement path — doesn't work as-is: `LocalSetBuilder::finalize(ul_key, enforce_mandatory)` always appends a fresh checksum as **tag 1** (`kChecksumTag = 1`, `include/misbklv/builder.hpp`) after its mandatory check, which collides with ST 0102's own tag 1, Security Classification. `finalize` is for top-level (checksummed) Local Sets; nesting uses `serialize_items()` instead (`test/nested_roundtrip_test.cpp`'s `rebuild_items`), which does no mandatory check at all.

So `security0102.toml` **does** get `flags = ["mandatory"]` on tags 1, 2, 3, 12, 13, 22, but the checking is split out from checksum emission: a new `LocalSetBuilder::check_mandatory() const` (see implementation list above) runs `finalize`'s existing loop without appending anything. A caller authoring a Security LS to nest under tag 48 calls `check_mandatory()` before `serialize_items()`; `finalize()` itself is refactored to call the same method rather than duplicating the loop. This is opt-in for nested builds — `serialize_items()` itself still does no check, matching every other nested registry's builder path — but it means "Required" now maps to something a caller *can* enforce, not just a comment.

# Alternatives considered

- **Type tag 13 by adding a `Utf16` `ValueKind`.** Rejected for now: it touches the shared descriptor schema, the codegen's `KIND` dict, and every consumer that switches on `ValueKind` — a cross-cutting change to serve one field in one registry.
- **A free-standing `decode_utf16be(span) -> std::string` helper next to the registry, keeping `kind = "bytes"`.** This is the cheapest real option and genuinely worth doing — it needs no schema, codegen, or `ValueKind` change, just a small library function callers can reach for. Not included in *this* ADR's scope (which is the registry/typing shape), but noted here so the follow-up implementation PR can add it as a small addition rather than reinventing the question.
- **Decode tag 13 by transcoding UTF-16 → UTF-8 in the codegen/codec, keeping `kind = "utf8"` for it.** Rejected: `Utf8` elsewhere in this schema means "the bytes are already UTF-8," not "decode-and-convert," and UTF-16 bytes routinely contain embedded NULs that would misbehave under UTF-8-oriented handling.
- **A dedicated `enum` `ValueKind` for tags 1/2/12's byte enumerations.** Rejected: no existing registry has one despite ST 0601 having its own enumerated-byte items, so this would be a new pattern introduced for ST 0102 alone. `uint` plus a documentation comment matches the house style.
- **Enforcing "Required" by reusing `finalize` as-is.** Rejected — see Decision; it collides with the checksum tag.
- **Leaving "Required" documentary only (a comment, no code).** Considered and initially proposed, but only because the obvious mechanism didn't work — not because enforcement is undesirable. Once `check_mandatory()` exists as a small, checksum-free extraction of logic `finalize` already has, there's no real cost to giving "Required" a working enforcement path instead of a comment.
- **Give `security0102.toml` a `ul_key`, matching a top-level registry.** Rejected: `Message::create` accepts any registry with a non-empty `ul_key` (`src/message.cpp`), which would make a standalone ST 0102 `Message` constructible — but `Message::set(1, …)` treats tag 1 as the checksum (`message.cpp`), so Security Classification could never be set, and `encode()` would then fail or emit a bogus checksum. Standalone parsing would also need a `kRegistries` entry (`src/registries.cpp`) this ADR doesn't propose. Leaving `ul_key` out, like `vtarget0903.toml`, keeps this nested-only and avoids a half-working standalone path.
- **Keep the whole LS opaque and only unwrap it at the application layer.** Rejected: that's the status quo the fork exists to change.

# Consequences

- Item descriptors for tag 48's contents become available for caller-driven descent (the same pattern `test/nested_roundtrip_test.cpp` already exercises for VMTI) — `Message::get(48)` itself is unchanged and still returns raw bytes; the library does not auto-decode nested sets.
- Tag 13 (Object Country Codes) remains a `bytes` passthrough — round-trips correctly, but callers wanting object-country codes must decode the UTF-16 payload themselves (a small helper is a natural, separate follow-up; see Alternatives).
- A caller authoring a Security LS who skips `check_mandatory()` before nesting it gets no complaint — the check is opt-in, matching how every other nested-registry builder path already works. One who does call it and omits tag 1, 2, 3, 12, 13, or 22 gets `Error::MissingMandatory`.
- `LocalSetBuilder::finalize()` changes internally (calls `check_mandatory()` instead of its own loop) but not observably — same checks, same error, same call sites.
- No change to how ST 0601 itself is decoded — `check_mandatory()` is an encode-side addition; nothing about reading an existing capture's tag 48 is affected.
- CI's generated-output drift check and the `regenerate-registry` build target must both learn about `security0102.toml`, or the new file silently never regenerates.

# Assumptions / open questions

- **The Universal Set form (ST 0102 §6.6) is out of scope**, per ST 0601.14-31 confirming tag 48 always carries the LS form.
- **`check_mandatory()` enforces one fixed reading of "Required"; ST 0102's own text is version-conditional.** §6.4 allows partial sets in general ("not all metadata elements... may be required"), tag 12 was optional before version 6, and -57 says an absent Version (tag 22) implies version 3 — while -56 says Version *shall* be included from version 4 onward. The six tags this ADR marks `mandatory` (1, 2, 3, 12, 13, 22) match Table 2's Required column for the *current* revision (ST 0102.12); a caller authoring against an older version's rules (e.g. treating tag 12 as optional) would need to skip `check_mandatory()` or we'd need a version-aware variant — neither exists here. This only affects authoring; nothing about decoding an existing capture is affected, and it's a reasonable follow-up if a caller actually hits it.
- **Tags 15–21 are undefined in ST 0102.12.** A stream built against an older revision could carry data on those tags; this registry doesn't reserve or reject them, matching how other registries in this codebase treat unregistered tags.
- **Byte-enumeration values (tags 1, 2, 12) are recorded as TOML comments, not validated.** A value outside the standard's table decodes as whatever raw `uint` it is; nothing here rejects an unrecognized classification byte, mirroring ST 0601's own enumerated items.
- **Whether real encoders ever write tag 13 as plain ASCII (which would decode fine as `utf8` despite the standard specifying UTF-16) is unconfirmed** — worth checking against real captures or another implementation (e.g. jmisb) before or alongside the implementation PR, not assumed here.

# Citations

[1] [ADR 0010](./0010-registry-descriptor-schema.md) — the `NestedLS`/`Pack` `ValueKind` variants and childRegistry routing this reuses.
[2] [ADR 0012](./0012-registry-codegen.md) — the TOML source format and codegen this extends with a new registry file.
[3] `registry/uas0601.toml` tag 74 (`kind = "nested_ls"`, `child = "vmti_0903"`) — the direct precedent for this fork's shape, exercised by `test/nested_roundtrip_test.cpp`.
[4] ST 0102.12 §6.7, Table 2 — the Local Set item table this ADR's per-item typing decisions are read from.[^st0102]
[5] ST 0601.19 §14-31 — confirms tag 48 always carries the Local Set (not Universal Set) form.

[^st0102]: ST 0102.12 §6.7 / Table 2 — Security Metadata Local Set Elements: tag numbers, data types, and Required/Context/Optional column for every item.
