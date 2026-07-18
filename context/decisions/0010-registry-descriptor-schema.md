---
type: Decision
title: Registry descriptor schema
status: accepted
tags: [decision, architecture, registry, schema, codegen, phase-3]
timestamp: 2026-07-17T21:30:00Z
fork: 8
---

# Context

Fork 8. ADR [`0006`](./0006-tag-registry.md) decided the registry is
**compiled-in `constexpr`, generated from a source-of-truth at build time** but
explicitly punted *what a descriptor contains* — the field set every codec
(read **and** write), the typed view ([`0005`](./0005-klv-core-data-model.md)),
and the codegen consume. That schema is the keystone: nothing in Phase 3 can be
written concretely until it exists.

The Phase 3 extraction spike (2026-07-17, `Day Flight.mpg`; see
[`log`](../log.md)) grounded the requirements in real bytes and corrected a KB
error — 0601's core numeric items are **not** uniformly IMAPB. The schema must
therefore capture, per item:

- **Two distinct numeric mapping kinds** ([`st0601`](../st0601.md) § Encoding):
  a **legacy linear (LDS)** map (e.g. Item 13 Sensor Latitude — `int32`,
  `-((2^31)-1)..(2^31)-1` → ±90°, §8.13) *and* **ST 1201 IMAPB** (extended
  items, tags ~90+). A single "numeric" type is insufficient.
- **Per-item special values** (§7.5): `0x80000000`="Reserved" for lat/lon,
  Out-of-Range, N/A (Off-Earth), plus IMAP `BELOW_MINIMUM`/`ABOVE_MAXIMUM`.
- **Fixed vs variable length** (spike: Item 2 fixed 8 B; Items 11/12 variable
  strings; IMAPB items are `V`).
- **Nesting / scoping** ([`st0601`](../st0601.md) § Nesting): an item's value
  may be a child Local Set (Item 74 → [`0903`](../st0903.md) VMTI) whose tags
  are scoped to a *different* registry. And [`0903`](../st0903.md) § Encoding:
  the VMTI LS uses BER-OID tags (byte 6 = `0x2B`) while its embedded LSs
  (VObject, VMask, …) use 1-byte UINT tags (byte 6 = `0x03`) — tag **encoding**
  is a per-registry property, and tag numbers are only unique *within* a
  registry.

# Decision

Define the descriptor schema as a **flat, `constexpr`-friendly aggregate per
item, grouped into per-registry tables** — data-driven, consumed by a small
fixed set of shared codecs (not one function per item).

## Value kind (the discriminator)

```cpp
enum class ValueKind : uint8_t {
  UInt, Int,        // raw big-endian integer, no mapping
  LinearLDS,        // legacy 0601 linear int->float map (params.min/max)
  IMAPB,            // ST 1201 mapping (params.min/max + length)
  Utf8, Bytes,      // string / opaque
  NestedLS,         // value is a child Local Set -> childRegistry
  Pack,             // VLP/DLP/FLP (incl. 0903 Array/Series) -> childRegistry
};
```

`ValueKind` doubles as the tag set of the typed-view variant, resolving an open
question in [`0005`](./0005-klv-core-data-model.md): the variant is
`{ uint64, int64, double (mapped), string_view, span<const byte>, NestedSet }`.

## Per-item descriptor

```cpp
struct MappingParams { double min, max; };        // LinearLDS / IMAPB only

struct SpecialValue {                             // may be several per item
  uint64_t     pattern;                           // raw KLV bits, width = length
  SpecialKind  kind;                              // Reserved, NotApplicable,
};                                                //   BelowMin, AboveMax, UserDefined

struct LengthSpec { bool variable; uint8_t fixed; uint8_t maxVar; };

struct ItemDescriptor {
  uint16_t                       tag;             // logical tag; BER-OID enc by codec
  std::string_view               name;
  std::string_view               units;           // may be empty
  ValueKind                      kind;
  LengthSpec                     length;
  MappingParams                  map;             // ignored unless a mapped kind
  std::span<const SpecialValue>  specials;        // constexpr array; may be empty
  RegistryId                     childRegistry;   // valid iff Nested/Pack, else None
  uint8_t                        flags;           // Mandatory | MultiUse | Deprecated
};
```

## Registry

```cpp
enum class TagEncoding : uint8_t { BerOid, OneByteUint };

struct Registry {
  RegistryId                        id;
  TagEncoding                       tagEncoding;   // 0601/0903-LS = BerOid; embedded = OneByteUint
  std::span<const std::byte>        ulKey;         // 16-B UL for top-level sets; empty for embedded
  std::span<const ItemDescriptor>   items;         // sorted by tag (binary search)
};
```

`RegistryId` is a closed `enum` over the v1 registries (0601 UAS LS, 0903 VMTI
LS, and 0903's embedded LSs). Nested/pack items route to a child by
`childRegistry`, so the recursive-aware core ([`0005`](./0005-klv-core-data-model.md))
knows which table to descend into.

**Scope of this ADR:** the *logical schema* and the *generated C++ shape* the
codegen emits and the codecs consume. The human-authored **source-of-truth file
format** (the open item from [`0006`](./0006-tag-registry.md)) and the codegen
tool are a small follow-on — recommendation carried below, not fixed here.

# Alternatives considered

- **Flat all-fields struct (chosen)** — a few unused fields per row (e.g. `map`
  on a string item); trivial for a ~143-row table, maximally `constexpr`-simple,
  trivial to generate and to binary-search.
- **Tagged union / `std::variant` per kind** — tighter, but non-trivial-type
  variants in a `constexpr` table add codegen and literal-type friction for no
  meaningful memory win at this scale. Rejected.
- **Polymorphic descriptor classes (one per kind, vtables)** — reintroduces the
  per-item-class anti-pattern [`0006`](./0006-tag-registry.md) rejected; not a
  compile-time constant. Rejected.
- **Single global tag table** — breaks on 0903's embedded LSs, which reuse small
  tag numbers with different meaning; scoping *requires* per-registry tables.
  Rejected in favour of per-registry `Registry` records.
- **Per-item codec function pointers** — encode the mapping as a function, not
  data. Rejected: defeats "data-driven"; a handful of shared codecs parameterized
  by `MappingParams` cover every item.
- **Special values as a side lookup keyed by tag** — rejected for locality; an
  inline `std::span<const SpecialValue>` on the descriptor keeps them together.

# Consequences

- **Unblocks the rest of Phase 3.** Typed view (variant = `ValueKind`), the
  codec set (raw-int / linear / IMAP / utf8 / bytes / recurse — parameterized by
  the descriptor), and the codegen target are all now concrete.
- **Codegen emits**, per registry: a tag-sorted `ItemDescriptor[]`, the backing
  `SpecialValue[]` arrays, and the `Registry` record; plus the `RegistryId`
  enum. Adding an item = a source row; adding a standard = a new registry.
- **Encode path ([`0011`](./0011-encode-model.md), fork 9) shares the schema** —
  `MappingParams` drives forward *and* reverse mapping; `LengthSpec`/`flags`
  drive length and mandatory-item emission. The schema is direction-neutral.
- Binary-search lookup by tag (sorted table) — no runtime allocation, matches
  the `constexpr` intent of [`0006`](./0006-tag-registry.md).

# Assumptions / open questions

- **IMAPB length vs precision:** decode length is known from the TLV; encode
  needs length (or precision → length). Storing `length` (via `LengthSpec`)
  suffices; IMAPA precision is derivable. Confirm when the first IMAPB item is
  implemented.
- **Special-value width:** `pattern` stored as `uint64_t`; the item length gives
  the true width. Adequate through 8-byte items.
- **Pack sub-structure** (VLP/DLP/FLP distinction; 0903 Array vs Series) — modeled
  as `ValueKind::Pack + childRegistry` for now; the pack *flavor* field may be
  needed when 0903 Series/Arrays are implemented. Deferred to that point.
- **Source-of-truth format + codegen tool** (from [`0006`](./0006-tag-registry.md)):
  lean is a single TOML/JSON file + a Python generator (editable without C++,
  validatable). Settle as a follow-on before the table is authored.
- **`RegistryId` as enum vs index** — settle in implementation.
- **Validation of this schema:** locked by a byte-exact round-trip of the spike's
  first packet (Items 2, 5–7, 11–15, 1/checksum) once [`0011`](./0011-encode-model.md)
  lands.

# Citations

[1] [`0006`](./0006-tag-registry.md) — compiled-in `constexpr`; punted the schema
    (this ADR completes it).
[2] [`0005`](./0005-klv-core-data-model.md) — typed-view variant (pinned here by
    `ValueKind`); recursive-aware core (uses `childRegistry`).
[3] [`st0601`](../st0601.md) § Encoding / § Nesting; ST 0601.19 §8.13 (Item 13
    linear map), §7.5 (special values).
[4] [`st1201`](../st1201.md) — IMAPB mapping + special values.
[5] [`st0903`](../st0903.md) § Encoding — BER-OID (`0x2B`) vs 1-byte-UINT
    (`0x03`) tag encoding; embedded LS scoping.
[6] [`st0107`](../st0107.md) — BER-OID tags, BER length.
[7] Phase 3 extraction spike, [`log`](../log.md) 2026-07-17 (real-byte grounding).
