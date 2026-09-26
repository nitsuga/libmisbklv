---
type: Decision
title: ST 1601 Geo-Registration LS as a typed nested registry, with a new MDARRAY (ST 1303) decode path
decision_status: accepted
tags: [decision, registry, 1601, 1303, mdarray, geo-registration, nested-ls, phase-3]
generated:
  by: claude/sonnet-5
  at: 2026-09-26T05:22:39Z
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

**Why MDARRAY doesn't fit the existing schema.** This codebase's `ValueKind::Pack` (ST 0903's VTarget Series, tag 101) is a *repeated child-registry TLV sequence* — read a length, route to a child registry, decode named tags, repeat. MDARRAY is structurally nothing like that: it's a single value whose own header (dimension count, per-dimension sizes, element byte width, one of five "Array Processing Algorithm" codes, and optional algorithm-specific parameters) must be parsed before you know where the following element data ends, and that element data is homogeneous array content, not a sequence of tagged items. There is no "child registry" to route into.

**MDARRAY wire layout, confirmed against ST 1303.2 directly** (§7.2, Table 1/Figure 4): `NDim` (BER-OID) → `Dim1`...`Dim_NDim` (each BER-OID) → `EBytes` (BER-OID) → `APA` (BER-OID) → `APAS` (optional, format depends on `APA`) → Array of Elements (optional, whatever bytes remain in the value). Appendix D / Table 3 defines exactly 5 APA codes: `0x01` Natural (no processing, no APAS), `0x02` ST 1201 IMAPB (Element-Processed; APAS is one Minimum/Maximum pair per ST 1303-17/-19, length 8 or 16 bytes, both 32-bit or both 64-bit IEEE 754, its length found by subtracting every other field's length from the total pack length), `0x03` Boolean (Compact; no APAS; elements bit-packed, `ceil(count/8)` bytes), `0x04` Unsigned Integer (Compact; APAS is one BER-OID bias; elements are concatenated BER-OID values with no per-element boundary marker), `0x05` Run-Length (Compact; APAS is one fixed-`EBytes`-width default value; elements are a variable-count list of value+coordinate+run-length "patch" records). In every case the Array of Elements' *outer* boundary is unambiguous — "whatever remains in the value" — even though the *inner* structure of the two Compact algorithms (0x04, 0x05) requires sequential decode to enumerate individual elements.

**A real ambiguity this ADR does not fully resolve: which APA does ST 1601 actually put on the wire for items 9 and 10?** Their own text (§6.3.9/§6.3.10) documents *different* IMAPB ranges per array row — e.g. σ uses `IMAPB(0,100,EBytes)`, ρ uses `IMAPB(-1,1,EBytes)` — which looks at first like it needs per-row APAS parameters. A single pack-level APAS pair can't reproduce that literally (one (min,max) can't be simultaneously (0,100) and (-1,1)), and ST 1303's own Example 3 (§8.2) says using the pack-level APA=2 for a heterogeneous array "is not recommended... because it is impeded by the need to represent values with ranges orders of magnitude apart using a single mapping" — precisely items 9/10's situation — though §8.1 notes APA=2 on a heterogeneous array is still technically permitted with "care in choosing the APAS parameters," so this is a strong steer, not an absolute prohibition. The far more plausible reading is that items 9/10 use **APA=1 (Natural)**, with each element already an ST 1201 IMAPB-mapped integer that the *item's own documented per-row range* (not the generic MDARRAY pack) is needed to decode — consistent with NoteC/NoteD's "EBytes is the max length across all IMAP'ed sigma and rho values" (which only makes sense if each row is independently mapped against its own range, then packed at a shared width). **This ADR does not have a real capture or cross-implementation reference (e.g. jmisb) to confirm this reading**, so the design below deliberately doesn't hardcode it — see Decision and Assumptions.

# Decision

**Add `ValueKind::MdArray` and a dedicated `misbklv::mdarray` module for structural parsing, exposing typed accessors for the algorithms and element types this codebase can identify from the wire itself — not a hardcoded assumption about what ST 1601 items 9/10 use.** Add a new nested registry `GEO_REGISTRATION_1601` and change ST 0601 tag 98 from `bytes` to `nested_ls` pointing at it, following fork 36/37's pattern exactly, including every piece of standard wiring that pattern needs:

- `registry/georegistration1601.toml` (new), `registry/uas0601.toml` tag 98 → `nested_ls`/`child = "geo_registration_1601"`.
- `tools/gen_registry.py`: add `"mdarray": "MdArray"` to the `KIND` dict (or `validate()` rejects any item using it), and `"geo_registration_1601": "GeoRegistration1601"` to `CHILD`.
- `include/misbklv/types.hpp`: add `GeoRegistration1601` to `RegistryId`, and `MdArray` to `ValueKind`.
- `include/misbklv/registries.hpp` / `src/registries.cpp`: the generated-header include and a `registry_for()` case for `GeoRegistration1601` — the same plumbing every prior nested registry needed. (`ValueKind::MdArray` itself needs none of this — it has no child registry, so nothing routes through `registry_for()` for it. Confirmed directly: `ItemDescriptor::child` is only read by `registry_for()`, which `MdArray` items never populate.)
- `src/codec.cpp`: **`decode()` needs no change** — its `default:` branch already returns the raw byte span for any `ValueKind` it doesn't special-case (confirmed by reading the function directly; this is how `Bytes`/`NestedLS`/`Pack` already work). **`encode()` does need a one-line change**: unlike `decode()`, `encode()` has no `default:` — it falls through to `TypeMismatch` for any kind not explicitly listed. Add `case ValueKind::MdArray:` to the existing `Bytes`/`NestedLS`/`Pack` passthrough group, or authoring/round-tripping an MDARRAY item's raw bytes via `LocalSetBuilder::set()`/`Message::set()` fails outright.
- `CMakeLists.txt`: `src/mdarray.cpp` needs adding to the library's source list (alongside `src/codec.cpp`), and `georegistration1601.toml` needs its own `regenerate-registry` codegen command, the same as every prior registry TOML.
- `.github/workflows/ci.yml`: add `georegistration1601` to the hardcoded drift-check loop (`for r in uas0601 vmti0903 vtarget0903 security0102 compositeimaging1602; do ...`) — every prior registry TOML is in this list; omitting the new one means CI never checks it for generated-output drift.

**The `mdarray` module** (`include/misbklv/mdarray.hpp`, `src/mdarray.cpp`):

```cpp
namespace misbklv::mdarray {

enum class Apa : std::uint8_t {
  Natural = 1,         // no processing; raw fixed-width elements
  Imapb = 2,            // ST 1201 IMAPB per element; APAS = one (min, max) pair
  BooleanCompact = 3,   // bit-packed; APAS is empty
  UintCompact = 4,      // BER-OID elements; APAS always holds the bias (a BER-OID,
                         // even when the bias itself is 0 -- never absent)
  RunLength = 5,        // patch-list encoding; APAS carries the default value
};

struct MdArray {
  std::vector<std::uint64_t> dims;      // NDim entries, row-major (ST 1303 §7.2.6.1)
  std::uint32_t ebytes;                  // widened; EBytes is BER-OID, not inherently <= 255
  Apa apa;
  std::span<const std::byte> apas;      // raw, format depends on apa (empty for Natural/Boolean)
  std::span<const std::byte> elements;  // raw "Array of Elements" bytes -- always well-defined

  std::size_t element_count() const;    // product of dims -- always >= 1 (ST 1303-09: every
                                         // Dim_i >= 1), regardless of whether there's data
  bool has_data() const;                // false iff EBytes == 0 -- the
                                         // shape (dims) is still meaningful even with no data

  // Typed access. `parse()` accepts any ebytes width for Natural/IMAPB --
  // width validity is meaningless without knowing which accessor a caller
  // means to use (an ebytes of 3 is a perfectly good Natural uint, but not a
  // valid element_float()). So each accessor validates its own precondition
  // (right apa, ebytes in [1,8] for element_uint/element_imapb, ebytes in
  // {4,8} for element_float) and returns Result rather than asserting --
  // apa/ebytes both come straight off the wire, so a mismatch here is
  // untrusted-input territory, not an internal invariant this codebase
  // already checked (the usual basis for an assert elsewhere in this code).
  Result<std::uint64_t> element_uint(std::size_t flat_index) const;  // Natural, big-endian
  Result<double> element_float(std::size_t flat_index) const;         // Natural, IEEE 754
  Result<double> element_imapb(std::size_t flat_index) const;         // Imapb: (min, max) from apas
};

// Parses NDim, every Dim_i, EBytes, APA, and (per APA's own rule) APAS, then
// takes the remainder of `value` as the elements span. Structurally correct
// for all 5 APAs -- the outer elements boundary never requires interpreting
// the elements themselves, even for the two Compact algorithms whose inner
// per-element boundaries this module doesn't enumerate. For APA=2 (IMAPB)
// specifically, the split between APAS and elements isn't "whatever's left
// after a fixed-size APAS" -- it's the reverse: elements' length is computed
// first (element_count() * ebytes, per §7.2.6.1's Natural/Element-Processed
// formula), and APAS is whatever remains before that (checked to be exactly
// 8 or 16 bytes, per ST 1303-19).
//
// Rejects (BadLength / OutOfRange, not UB) rather than silently misparsing:
//  - NDim inconsistent with remaining bytes (each Dim_i needs >= 1 byte;
//    bounds `dims` growth by input length and fast-fails an impossible header).
//  - Any arithmetic overflow computing element_count() or
//    element_count() * ebytes (both checked with overflow-safe multiplication,
//    not `std::size_t` wraparound) -- and, for APA=2, an *underflow* guard:
//    the bytes remaining after the APA field must be at least
//    element_count() * ebytes before subtracting to find APAS's length, or a
//    crafted short value must fail cleanly instead of wrapping to a huge
//    APAS length.
//  - An APA value outside 1-5 (0x00 is reserved per ST 1303 Table 3).
//  - **Wrong elements length for the two algorithms with an exact, checkable
//    one** (not just "whatever remains" -- these two get validated, not just
//    accepted): Natural (APA=1) must have exactly
//    `element_count() * ebytes` bytes of elements (ST 1303 §7.2.6.1);
//    Boolean (APA=3) must have exactly `ceil(element_count() / 8.0)` bytes
//    (ST 1303 Appendix D.2). A value with extra or missing trailing bytes
//    under either APA is malformed, not silently truncated/padded.
//
// EBytes == 0 is a real, legal case with a specific meaning per ST 1303
// §7.2.3: it signals an *empty* Array of Elements regardless of APA (a
// "no data"/"no change" marker), not a placeholder for variable-length
// elements -- that placeholder role belongs to EBytes == 1 instead, used by
// the two Compact algorithms that fix EBytes at 1 (UintCompact's BER-OID
// elements, Boolean's bit-packed ones). `parse()` treats EBytes == 0 as
// `elements` being empty (`has_data() == false`) for every APA except 0x02 --
// dims/element_count() stay meaningful (shape without data), it's only the
// element bytes that vanish. **APA=2 is the one exception, and does not get
// an EBytes==0 carve-out**: ST 1303-17/-19 are SHALL requirements that APAS
// always contain a Minimum/Maximum pair sized 8 or 16 bytes, with no stated
// exception for an empty array -- so `parse()` still requires a valid 8- or
// 16-byte APAS under APA=2 regardless of EBytes, and rejects anything else
// (including an empty one) as malformed rather than inferring a permissive
// reading the standard's own SHALL language doesn't support. IMAPB APAS bounds
// are decoded from the wire: non-finite bounds can yield a non-finite accessor
// result, which callers needing finite values must reject themselves.
Result<MdArray> parse(std::span<const std::byte> value);

}  // namespace misbklv::mdarray
```

**Item 4's element type is fixed by ST 1601's own text**: its notation is `MDARRAY(NoteA, 2, 4, NoteB)`, and NoteA explicitly overrides ST 0807's registered Data Type for the underlying row/column keys from BER-OID to UINT (§6.3.4) — `element_uint()` covers item 4 directly. **Items 5, 8, 9, 10's element type is not fully pinned down by the standard text alone**: items 5 and 8 (latitude/longitude/elevation) take their type from external ST 0807-registered keys the standard doesn't reproduce, and could be Natural float or IMAPB depending on encoder choice (ST 1303's own Example 2 language: "run-time" type selection is normal for MDARRAY); items 9/10 are almost certainly Natural-with-item-documented-per-row-IMAPB-ranges per the Context discussion, but this ADR doesn't assert that as fact. **The design responds to this by exposing `apa`/`ebytes`/`dims` on every `MdArray` and providing `element_uint`/`element_float`/`element_imapb` as generic accessors a caller (or a later, smaller ST-1601-specific helper) chooses between based on what's actually on the wire** — not by guessing one specific encoding into the ADR and being wrong. This mirrors how MDARRAY is designed to be self-describing in the first place.

**Per-item typing for the other 5 ST 1601 items**, from Table 1:

| Tags | Kind | Notes |
|---|---|---|
| 1 (Document Version) | `uint`, `variable = true` | Mandatory. Plain `uint` per Table 1 — app-defined width, same treatment ADR 0041 gave ST 1602's tag 2. |
| 2 (Algorithm Name), 3 (Algorithm Version) | `utf8`, variable | Both Mandatory. |
| 6 (Second Image Name) | `utf8`, variable | Optional. |
| 7 (Algorithm Configuration Identifier) | `bytes`, `fixed_len = 16` | Optional. RFC 4122 UUID, 16 raw bytes. **`fixed_len = 16` is documentary, not enforced** — `codec::decode`/`encode` treat every `bytes` item identically (return/copy whatever span is there, per-length or not), the same as `fixed_len` on any other `bytes` item in this codebase's registries; nothing rejects a mis-sized value at this tag today. No version constraint is wire-enforced either (ST 1601 recommends v4/v5 but doesn't mandate it). No new `ValueKind` needed — this is the same opaque-bytes treatment this codebase already gives other 16-byte binary identifiers (e.g. the ST 0604 SEI UUID in `src/gst/gst_video.cpp`). |

**Mandatory flags: tags 1, 2, 3** (`flags = ["mandatory"]`), matching Table 1's Rules column. Tags 4-10 are all Optional (a Geo-Registration LS can legitimately carry only the algorithm identification, with no tie-point data at all). `LocalSetBuilder::check_mandatory()` (from ADR 0040) applies unchanged.

**Cross-item constraint left unenforced.** ST 1601.1-03 requires tags 4, 5, 8, 9, 10 to use the same tie-point count when present together — for the 2-D items (4, 5, 9, 10) that count is `Dim2`; for item 8 (1-D: `MDARRAY(NoteA, 1, NoteB)`) it's `Dim1`. This is a cross-item, cross-array-shape constraint `check_mandatory()` has no mechanism for (it checks tag presence, not relationships between decoded array dimensions) — recorded as a TOML comment, matching how ST 1602's Z-Order uniqueness rule (ADR 0041) and ST 0102's Version-conditional requirements (ADR 0040) were already left as caller-enforced, not library-enforced.

# Alternatives considered

- **Keep tags 4/5/8/9/10 opaque `bytes`, type only tags 1/2/3/6/7.** Rejected: unlike ST 0102's single UTF-16 field, the MDARRAY items *are* what ST 1601 exists for — tie-point correspondence data is the standard's entire substance.
- **Model MDARRAY as a `Pack` with a synthetic child registry.** Rejected — see Context. `Pack` assumes repeated tagged TLV items; MDARRAY is a self-describing header plus homogeneous (or algorithmically-packed) element data. There's no tag to dispatch on inside an MDARRAY value.
- **Hardcode item 9/10 decode as "one global IMAPB range from APAS."** Rejected — this was this ADR's own first draft, and it's wrong: ST 1303 itself discourages exactly this (one mapping across orders-of-magnitude-different per-row ranges), and NoteC/NoteD's "max EBytes across all IMAP'ed sigma/rho values" language only makes sense if each row is mapped against its own range before a shared width is chosen — which a single pack-level APAS pair cannot represent.
- **Auto-apply ST 1601's per-row IMAPB ranges inside the generic `mdarray` module** (a `decode_st1601_item9(MdArray)`-style helper baked into the ST-1303-generic module). Rejected for *this* ADR: it bakes one invoking standard's item-specific semantics into what should be a standard-agnostic ST 1303 module, and the exact encoding items 9/10 use isn't confirmed (see Context). A small, separate, ST-1601-specific convenience layer on top of the generic `mdarray` module is a reasonable smaller follow-up once a real capture or cross-implementation reference confirms the encoding — not part of this fork.
- **Full typed decode for all 5 APAs, including Boolean/UInt-compact/Run-Length element interpretation.** Rejected for now: nothing in this codebase's target standards uses these three for anything currently in scope, and each would need meaningfully different, untested decode logic built against zero real usage. Structural parsing (correct byte boundaries, safe to skip/round-trip) is provided for all 5 — only *element interpretation* is deferred for the three unused ones.
- **A typed encode/authoring API for MDARRAY** (constructing a new array from scratch, choosing APA, etc.). Rejected: every consumer of this library so far decodes existing telemetry; nobody has asked to synthesize new geo-registration data. Encoding an MDARRAY item goes through the existing raw-bytes path (`append_raw`, and now `LocalSetBuilder::set()` once `encode()` gains its `MdArray` case), the same way every nested LS body is built today.

# Consequences

- Tags 4, 5, 8, 9, 10 become structurally parseable — a caller gets dimension shape, element count, algorithm code, and (for Natural/IMAPB) typed element values, instead of an opaque blob. `Message::get(98)` itself is unchanged; a caller descends into tag 98 the same way ST 0102/ST 1602 already require.
- **This is real new schema surface, not a reapplication of an existing pattern** — the standard `RegistryId`/`registries.hpp`/`registries.cpp`/`gen_registry.py` wiring fork 36/37 needed still applies in full for `GeoRegistration1601`; the only thing genuinely new (and needing no `registries.cpp` work) is `ValueKind::MdArray` itself, since it has no child to route into. `codec::encode` needs its one-line addition or nothing can author/round-trip an MDARRAY item through the builder API.
- Items 5/8/9/10's *exact* typed decode (which of `element_uint`/`element_float`/`element_imapb` actually applies) is not fully settled by this ADR — a caller inspecting a real ST 1601 stream's `apa`/`ebytes` fields picks the right one; item 4 alone is unambiguous (its own text overrides the type to UINT).
- Tags using Boolean/UInt-compact/Run-Length APAs (theoretically possible on the wire, though ST 1601's own text implies Natural/IMAPB for its items) parse structurally — safe to detect shape and skip/round-trip — but their element bytes are exposed raw, not decoded.
- The library gains a genuinely reusable capability: ST 1206 (SAR Motion Imagery, currently a separate candidate fork) also uses MDARRAY — tag 22, Radar Cross Section Scale Factor, confirmed in `references/ST1206.1.txt`. Any future MDARRAY-using standard reuses `mdarray::parse()` directly.

# Assumptions / open questions

- **Whether ST 1601 items 9/10 (and, less certainly, 5/8) are actually encoded as Natural-with-item-documented-per-row-ranges is this ADR's central open question, not a settled fact.** ST 1303's own text (§8.2, Example 3) is the strongest available evidence, but this ADR has no real capture or a cross-implementation reference (e.g. jmisb's ST 1601 handling, not checked — no jmisb source was available to consult) to confirm it. The implementation PR should decode conservatively: expose `apa`/`ebytes`/`dims` and the generic typed accessors; add an ST-1601-item-specific convenience layer only once a real sample or cross-reference confirms which encoding producers actually use.
- **UInt-compact and Run-Length element decode are real, deferred work**, not merely hypothetical: `MdArray::elements` for those two APAs is exactly as useful as an opaque `bytes` item today (a caller must write their own decode). The difference from full opacity is that the *shape* (dims, ebytes, bias/default value) is already parsed and available.
- **ST 1601.1-03's cross-item tie-point-count consistency is unenforced**, per Alternatives/Decision.
- **UUID version (v4/v5) is unvalidated**, per ST 1601's own §6.3.7 language ("recommend" not "require").
- **Parser hardening is part of this design, not an implementation afterthought**: `parse()` must bound `NDim` against remaining bytes before growing `dims` (each dimension consumes at least one input byte, so growth is input-bounded), check `element_count()` and `element_count() * ebytes` for overflow (plus the APA=2 underflow case — see `parse()`'s spec above) rather than trusting `std::size_t` wraparound, and reject an out-of-range `APA` — all before touching wire-derived values arithmetically. Width validation for a specific accessor (e.g. `ebytes` too wide for `element_float()`) is each accessor's own job, not `parse()`'s, since `parse()` can't know which accessor a caller will use. This is the same class of guard `hardening_test` already owns for BER-OID tags and Report-on-Change packets elsewhere in this codebase.

# Citations

[1] [ADR 0010](./0010-registry-descriptor-schema.md) — the `ValueKind` enum and `ItemDescriptor` schema this extends with a new marker kind.
[2] [ADR 0040](./0040-st0102-nested-registry.md) — the nested-registry pattern (no `ul_key`, `check_mandatory()`) this fork reuses unchanged.
[3] [ADR 0041](./0041-st1602-nested-registry.md) — the second application of the same pattern, confirming it generalizes before this, the third, adds genuinely new schema surface.
[4] `src/codec.cpp` `decode()`'s `default:` branch vs. `encode()`'s lack of one — confirmed directly, not by analogy, that these two functions need different treatment for a new `ValueKind`.
[5] ST 1601.2 §6.2, Table 1, §6.3.1-§6.3.10 — the Local Set item table and MDARRAY parameter notes this ADR's per-item decisions are read from.[^st1601]
[6] ST 1303.2 §7.2 (pack structure), Appendix D (the 5 APAs), §8.2 Example 3 (the heterogeneous-array/APA=2 discouragement that resolves the items-9/10 puzzle, at least as the best available evidence).[^st1303]
[7] `references/ST1206.1.txt` — confirms ST 1206 (tag 22) is a second real MDARRAY consumer in this codebase's own reference set, cited in Consequences.

[^st1601]: ST 1601.2 §6.2 (UL key), Table 1 (item list, types, Rules column), §6.3.1-§6.3.10 (per-item MDARRAY notation and notes), ST 1601.1-03 (tie-point count consistency requirement).
[^st1303]: ST 1303.2 §7.2 (pack byte layout and length accounting), Appendix D / Table 3 (the 5 APA codes), Appendix D.1-D.4 (per-APA APAS/element format, worked examples), ST 1303-17/ST 1303-19 (IMAPB APAS requirements), §8.1 (Type/Data Identifier semantics, permits APA=2 on a heterogeneous array "with care"), §8.2 Example 3 (the same case called "not recommended" in practice).
