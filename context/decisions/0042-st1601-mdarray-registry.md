---
type: Decision
title: ST 1601 Geo-Registration LS as a typed nested registry, with a new MDARRAY (ST 1303) decode path
decision_status: proposed
tags: [decision, registry, 1601, 1303, mdarray, geo-registration, nested-ls, phase-3]
generated:
  by: claude/sonnet-5
  at: 2026-09-26T00:00:00Z
fork: 38
sources:
  - id: st1601
    resource: ../../references/ST1601.2.txt
    title: MISB ST 1601.2 §6.2, Table 1 — Geo-Registration Local Set
  - id: st1303
    resource: ../../references/ST1303.2.txt
    title: MISB ST 1303.2 — Multi-Dimensional Array Pack
---

# Context

Fork 38, the last of the three registry-typing forks split out of the UAS-platform coverage survey ([#95](https://github.com/nitsuga/libmisbklv/issues/95)); ST 0102 (fork 36, [ADR 0040](./0040-st0102-nested-registry.md)) and ST 1602 (fork 37, [ADR 0041](./0041-st1602-nested-registry.md)) are both accepted and implemented. ST 1601 was deliberately held back from that pair: 5 of its 10 items (tags 4, 5, 8, 9, 10) use MISB ST 1303's Multi-Dimensional Array Pack ("MDARRAY") — a self-describing, variable-shape wire format nothing in this schema resembles — and one item (tag 7) is a raw UUID.

Tag 98 of the ST 0601 registry (`registry/uas0601.toml`) carries the Geo-Registration Local Set today as `kind = "bytes"`. ST 1601.2 §6.2 / Table 1 defines its 16-byte UL key (`06 0E 2B 34 02 0B 01 01 0E 01 03 03 01 00 00 00`, CRC 39238) and 10 items (tags 1-10, all defined).

**Why MDARRAY doesn't fit the existing schema.** This codebase's `ValueKind::Pack` (ST 0903's VTarget Series, tag 101) is a *repeated child-registry TLV sequence* — read a length, route to a child registry, decode named tags, repeat. MDARRAY is structurally nothing like that: it's a single value whose own header (dimension count, per-dimension sizes, element byte width, one of five "Array Processing Algorithm" codes, and optional algorithm-specific parameters) must be parsed before you know where the following element data ends, and that element data is homogeneous array content, not a sequence of tagged items. There is no "child registry" to route into. Building on `Pack` would mean stretching a mechanism designed for one shape onto a completely different one.

**Confirmed against the standard, not assumed:**
- ST 1303 Appendix D / Table 3 defines exactly 5 Array Processing Algorithm (APA) codes: `0x01` Natural (no processing), `0x02` ST 1201 IMAPB (Element-Processed), `0x03` Boolean (Compact), `0x04` Unsigned Integer (Compact, BER-OID with optional bias), `0x05` Run-Length (Compact, patch list).
- ST 1601 items 9 and 10's own text (§6.3.9/§6.3.10) describes *different* IMAPB ranges per array row (e.g. σ uses `IMAPB(0,100,EBytes)`, ρ uses `IMAPB(-1,1,EBytes)`) — but ST 1303-17 requires the whole array's APAS to carry exactly *one* Minimum/Maximum pair, and ST 1303 §8.1 explicitly acknowledges this: a heterogeneous array's "Data Identifier" (per-row meaning) can vary while the encoded APA parameters apply uniformly, and "care in choosing the APAS parameters is important because the data may have different value ranges." So decode only ever needs one global `(min, max)` per MDARRAY value, applied identically to every element — the per-row "natural range" documentation in ST 1601 is informative for a human reader (like every other TOML comment in this codebase's registries), not a second wire-level parameter set. This resolved what initially looked like a contradiction; it isn't one.
- Every APA's Array-of-Elements section is *provably* the same shape: whatever bytes remain in the value after the header and APAS are consumed (ST 1303 §7.2's own length-accounting equation makes this explicit — Pack Length minus every preceding field's length). This holds for all 5 APAs, including the two Compact ones whose *internal* element structure is genuinely more work to interpret (UInt-compact is a solid block of sequentially-dependent BER-OID values with no way to locate an individual element without decoding from the start; RLE is a list of variable-count "patch" records). Locating the array's *outer* boundary never requires interpreting its *inner* structure.

# Decision

**Add `ValueKind::MdArray` and a dedicated `misbklv::mdarray` module for structural parsing, with typed element access for the two algorithms this codebase's own data actually uses (Natural, IMAPB) and byte-level access for the other three.** Add a new nested registry `GEO_REGISTRATION_1601` and change ST 0601 tag 98 from `bytes` to `nested_ls` pointing at it, following the exact pattern of fork 36/37.

**Why `codec::decode`/`codec::encode` need no changes.** `codec::decode`'s `default:` branch already returns the raw byte span for any `ValueKind` it doesn't special-case (this is how `Bytes`, `NestedLS`, and `Pack` are all handled today — confirmed by reading `src/codec.cpp` directly, not assumed by analogy). `ValueKind::MdArray` falls into that same default branch for free. And unlike `NestedLS`/`Pack`, an MDARRAY item has no child registry to route into — it's a self-contained leaf value — so `include/misbklv/registries.hpp`/`registries.cpp` need no changes either. The entire new surface is the `mdarray` module plus the registry/enum plumbing every prior fork has already established the pattern for.

**The `mdarray` module** (`include/misbklv/mdarray.hpp`, `src/mdarray.cpp`):

```cpp
namespace misbklv::mdarray {

enum class Apa : std::uint8_t {
  Natural = 1,         // no processing; raw fixed-width elements
  Imapb = 2,            // ST 1201 IMAPB per element; APAS = one (min, max) pair
  BooleanCompact = 3,   // bit-packed; APAS is empty
  UintCompact = 4,      // BER-OID elements, optional bias in APAS
  RunLength = 5,        // patch-list encoding; APAS carries the default value
};

struct MdArray {
  std::vector<std::uint64_t> dims;      // NDim entries, row-major (ST 1303 §7.2.6.1)
  std::uint8_t ebytes;
  Apa apa;
  std::span<const std::byte> apas;      // raw, format depends on apa (empty for Natural/Boolean)
  std::span<const std::byte> elements;  // raw "Array of Elements" bytes -- always well-defined

  std::size_t element_count() const;    // product of dims

  // Typed access -- only meaningful when apa is Natural or Imapb; both assert
  // on the wrong apa rather than silently returning nonsense (ADR 0011's
  // "trust internal invariants" pattern -- caller already knows apa from the
  // struct it's holding).
  std::uint64_t element_uint(std::size_t flat_index) const;      // Natural: big-endian, ebytes wide
  double element_imapb(std::size_t flat_index) const;             // Imapb: (min, max) from apas, per ST 1201
};

// Parses NDim, every Dim_i, EBytes, APA, and (per APA's own rule) APAS, then
// takes the remainder of `value` as the elements span. Structurally correct
// for all 5 APAs; does not interpret Boolean/UintCompact/RunLength element
// bytes beyond exposing them as a span.
Result<MdArray> parse(std::span<const std::byte> value);

}  // namespace misbklv::mdarray
```

Per-APA parsing rules, from ST 1303 Appendix D (used by `parse()`, verified against the standard's own worked examples in Figures 6-12):
- **Natural (`0x01`)**: no APAS. `elements.size() == element_count() * ebytes`.
- **IMAPB (`0x02`)**: APAS length = (bytes remaining after the APA field) − (`element_count() * ebytes`); ST 1303-19 requires this to resolve to exactly 8 (two 32-bit floats) or 16 (two 64-bit doubles) — `parse()` rejects anything else as malformed. `element_imapb(i)` decodes `apas` as the (min, max) pair per that width, then applies ST 1201 IMAPB with `ebytes` width to element `i`.
- **Boolean (`0x03`)**: no APAS. `elements.size() == ceil(element_count() / 8.0)` (bit-packed, ST 1303 §D.2). No element accessor provided — bit-unpacking is straightforward but nothing in this codebase's target standards uses it yet.
- **UInt-compact (`0x04`)**: APAS is a single BER-OID (the bias) — self-length-prefixed, so `parse()` reads exactly one BER-OID value and that's the whole APAS. `elements` is the remainder: a solid block of concatenated BER-OID-encoded values with no per-element boundary markers (ST 1303 §D.3) — genuinely requires full sequential decode to enumerate, which this ADR doesn't provide.
- **Run-Length (`0x05`)**: APAS is a single fixed-width value, `ebytes` bytes (the default/background value). `elements` is the remainder: a variable-count list of `[value(ebytes) | coord(BER-OID)×NDim | run_length(BER-OID)×NDim]` patch records (ST 1303 §D.4, Figure 11) — enumerating patches isn't provided.

**`ValueKind::MdArray` is a marker with no `.child`.** Unlike `NestedLS`/`Pack`, nothing routes through `registry_for()` for it — a caller who wants structured access calls `mdarray::parse()` directly on the item's raw bytes, the same way a caller descending into a `NestedLS` calls `parse_items()` directly rather than going through `codec::decode`.

**Per-item typing for the other 5 ST 1601 items**, from Table 1:

| Tags | Kind | Notes |
|---|---|---|
| 1 (Document Version) | `uint`, `variable = true` | Mandatory. Plain `uint` per Table 1 — app-defined width, same treatment ADR 0041 gave ST 1602's tag 2. |
| 2 (Algorithm Name), 3 (Algorithm Version) | `utf8`, variable | Both Mandatory. |
| 6 (Second Image Name) | `utf8`, variable | Optional. |
| 7 (Algorithm Configuration Identifier) | `bytes`, `fixed_len = 16` | Optional. RFC 4122 UUID, 16 raw bytes — no version constraint is wire-enforced (ST 1601 recommends v4/v5 but doesn't mandate it), so this is opaque bytes at the exact width, the same treatment this codebase already gives other 16-byte binary identifiers (e.g. the ST 0604 SEI UUID in `src/gst/gst_video.cpp`). No new `ValueKind` needed — a fixed 16-byte span is exactly what `Bytes` already is. |

**Mandatory flags: tags 1, 2, 3** (`flags = ["mandatory"]`), matching Table 1's Rules column. Tags 4-10 are all Optional (the standard doesn't require any tie-point data be present at all — a Geo-Registration LS can legitimately carry only the algorithm identification). `LocalSetBuilder::check_mandatory()` (from ADR 0040) applies unchanged.

**Cross-item constraint left unenforced.** ST 1601.1-03 requires tags 4, 5, 8, 9, 10 to use the same tie-point count when present together. This is a cross-item, cross-array-shape constraint `check_mandatory()` has no mechanism for (it checks tag presence, not relationships between decoded array dimensions) — recorded as a TOML comment, matching how ST 1602's Z-Order uniqueness rule (ADR 0041) and ST 0102's Version-conditional requirements (ADR 0040) were already left as caller-enforced, not library-enforced.

# Alternatives considered

- **Keep tags 4/5/8/9/10 opaque `bytes`, type only tags 1/2/3/6/7.** Rejected: unlike ST 0102's single UTF-16 field, the MDARRAY items *are* what ST 1601 exists for — tie-point correspondence data is the standard's entire substance. Typing everything except the substance isn't a meaningful "typed" fork.
- **Model MDARRAY as a `Pack` with a synthetic child registry.** Rejected — see Context. `Pack` assumes repeated tagged TLV items; MDARRAY is a self-describing header plus homogeneous (or algorithmically-packed) element data. There's no tag to dispatch on inside an MDARRAY value.
- **Full typed decode for all 5 APAs, including Boolean/UInt-compact/Run-Length element interpretation.** Rejected for now: nothing in this codebase's target standards uses these three (ST 1601 only uses Natural and IMAPB), and each would need meaningfully different, untested decode logic (bit-unpacking, sequential variable-length BER-OID decode, or patch-list reconstruction) built against zero real usage. Structural parsing (correct byte boundaries, safe to skip/round-trip) is provided for all 5 — only *element interpretation* is deferred for the three unused ones, and adding it later needs no re-fork, just a new method on `MdArray`.
- **A typed encode/authoring API for MDARRAY** (constructing a new array from scratch, choosing APA, etc.). Rejected: every consumer of this library so far decodes existing telemetry; nobody has asked to synthesize new geo-registration data. Encoding an MDARRAY item (when round-tripping or hand-assembling one from already-correct wire bytes) goes through the existing raw-bytes path (`append_raw`), the same way every nested LS body is built today — no new mechanism needed for that.
- **Per-row IMAPB ranges in `element_imapb()`** (to match ST 1601's item text literally). Rejected — see Context: the wire format only carries one (min, max) pair per array; there's nothing to select per-row even if the API wanted to.

# Consequences

- Tags 4, 5, 8, 9, 10 become structurally parseable — a caller gets dimension shape, element count, and (for Natural/IMAPB) typed element values, instead of an opaque blob. `Message::get(98)` itself is unchanged; a caller descends into tag 98 the same way ST 0102/ST 1602 already require (find the descriptor, `parse_items()`, then per-child-tag `mdarray::parse()` for the MDARRAY ones).
- Tags using Boolean/UInt-compact/Run-Length APAs (theoretically possible on the wire, though ST 1601's own text implies Natural/IMAPB for its items) parse structurally — safe to detect shape and skip/round-trip — but their element bytes are exposed raw, not decoded. This is a real, documented gap, not silent data loss: nothing is hidden, a future caller needing those algorithms builds on `MdArray`'s existing fields.
- The library gains a genuinely reusable capability: any future MISB standard using MDARRAY (ST 1303 is used across several MISB standards beyond ST 1601) can reuse `mdarray::parse()` directly, unlike a bespoke per-standard hack.
- No change to `codec.cpp`, `registries.cpp`, or `registries.hpp` — confirmed directly, not assumed, by reading `codec::decode`'s existing default-branch behavior.

# Assumptions / open questions

- **UInt-compact and Run-Length element decode are real, deferred work**, not merely hypothetical: `MdArray::elements` for those two APAs is exactly as useful as an opaque `bytes` item today (a caller must write their own decode). The difference from full opacity is that the *shape* (dims, ebytes, bias/default value) is already parsed and available, which is strictly better than nothing.
- **Whether real ST 1601 producers ever emit anything but Natural/IMAPB for tags 4/5/8/9/10 is unconfirmed.** The standard's item text (§6.3.4-§6.3.10) describes them in terms that only make sense for Natural or IMAPB (floating-point tie-point coordinates and uncertainties), so this is a reasonable bet, not a guarantee — nothing prevents a compliant encoder from choosing a Compact APA for any of them.
- **ST 1601.1-03's cross-item tie-point-count consistency is unenforced**, per Alternatives/Decision — a caller who decodes tags 4, 5, 8, 9, 10 and finds mismatched tie-point counts (from `element_count()` per item) has found a genuinely non-compliant stream; this library doesn't reject it, matching this codebase's general posture of decoding what's on the wire rather than validating every cross-field rule a standard states.
- **UUID version (v4/v5) is unvalidated**, per ST 1601's own §6.3.7 language ("recommend" not "require") — `bytes`/16 accepts any 16-byte value regardless of the RFC 4122 version bits.

# Citations

[1] [ADR 0010](./0010-registry-descriptor-schema.md) — the `ValueKind` enum and `ItemDescriptor` schema this extends with a new marker kind.
[2] [ADR 0040](./0040-st0102-nested-registry.md) — the nested-registry pattern (no `ul_key`, `check_mandatory()`) this fork reuses unchanged.
[3] [ADR 0041](./0041-st1602-nested-registry.md) — the second application of the same pattern, confirming it generalizes before this, the third, adds genuinely new schema surface.
[4] `src/codec.cpp` `decode()`'s `default:` branch — confirmed directly that `NestedLS`/`Pack`/`Bytes` (and now `MdArray`) all fall through to raw-span decode with no per-kind special casing needed.
[5] ST 1601.2 §6.2, Table 1, §6.3.1-§6.3.10 — the Local Set item table and MDARRAY parameter notes this ADR's per-item decisions are read from.[^st1601]
[6] ST 1303.2 §7.2 (pack structure), Appendix D (the 5 APAs), §8.1 (heterogeneous-array Data Identifier semantics, resolving the apparent per-row-IMAPB-range contradiction).[^st1303]

[^st1601]: ST 1601.2 §6.2 (UL key), Table 1 (item list, types, Rules column), §6.3.1-§6.3.10 (per-item MDARRAY notation and notes), ST 1601.1-03 (tie-point count consistency requirement).
[^st1303]: ST 1303.2 §7.2 (pack byte layout and length accounting), Appendix D / Table 3 (the 5 APA codes), Appendix D.1-D.4 (per-APA APAS/element format, worked examples), ST 1303-17/ST 1303-19 (IMAPB APAS requirements), §8.1 (Type/Data Identifier semantics for heterogeneous arrays).
