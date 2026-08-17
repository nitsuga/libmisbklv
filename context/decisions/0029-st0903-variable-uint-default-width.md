---
type: Decision
title: ST 0903 variable-length uint default encode width
decision_status: accepted
tags: [decision, registry, codegen, st0903, encode]
generated:
  by: deepseek-v4-pro
  at: 2026-08-17T04:00:00Z
fork: 26
sources:
  - id: st0903
    resource: ../../references/ST0903.6.pdf
    title: MISB ST 0903.6 §9.1, Tables 9-10 — the "Vmax" variable-length uint notation and the affected items
  - id: st0601
    resource: ../../references/ST0601.19.pdf
    title: MISB ST 0601.19 — the existing variable-uint registry precedent (items 110/111/131/133)
---

# Context

Fork 26 (issue #5). ST 0903 VMTI tags 4/5/6 (`vmtiLsVersionNum`,
`totalNumTargetsDetected`, `numTargetsReported`) and VTarget tag 1
(`targetCentroid`) are variable-length unsigned integers. ST 0903.6 §9.1
defines the "Vmax" notation — a valid width anywhere in `[1, max]` — with
Table 9 giving `V2`/`V3` and Table 10 `V6`. These four items were registered
with neither `length` nor `variable`, so `gen_registry.py` emitted
`variable = true, fixed_len = 0`. `Message::set` and `LocalSetBuilder::set`
take the encode width from `fixed_len`, get 0, and `codec::encode` rejects
width 0 for numeric kinds (`Error::BadLength`, `valid_numeric_width`).
Authoring any realistic VMTI packet through the typed API is therefore
impossible; the round-trip tests only pass by explicitly passing
`it.value.size()`.

The library already has the exact precedent this fork needs. ST 0601 registers
four variable-uint items — 110 Time Airborne, 111 Propulsion Unit Speed, 131
Take-off Time, 133 On-board MI Storage Capacity — as `length = <max>,
variable = true`, where `length` is the **default encode width** (the file's
own header comment: "the DEFAULT ENCODE WIDTH … `variable = true` records that
the standard lets a sender pick another"). The 0903 rows are simply missing
that field.

Decoding never reads `fixed_len` or `variable` — it takes the actual field
length from the TLV — so this decision cannot shift the read path.

# Decision

Give each ST 0903 variable-uint item a canonical default encode width equal to
its standard maximum, marked `variable = true`, mirroring the 0601 precedent
exactly:

- VMTI tag 4 VMTI LS Version Number → **2** (`V2`; §10.1.4 "1 through 65535")
- VMTI tag 5 Total Number of Targets Detected → **3** (`V3`)
- VMTI tag 6 Number of Reported Targets → **3** (`V3`)
- VTarget tag 1 targetCentroid → **6** (`V6`; §10.2.2.2 "1 to 0xFFFFFFFFFFFF")

The default is the **max** width because it is the only single width that
accepts every valid value (a narrower default would `RangeError` on larger
values), and because it matches the 0601 variable-uint behavior, which is
already "always valid, never minimal". No API, codec, or generator change:
the registry `.toml` gains `length` + `variable`, and `gen_registry.py`
regenerates the committed tables.

# Consequences

- `Message::set(tag, value)` and `LocalSetBuilder::set(tag, value)` now work
  for the four items with no width argument, encoding at the standard's
  maximum width — conformant (width ∈ `[1, Vmax]`), not minimal, and not
  byte-identical to the standard's worked examples, which use the minimum
  width that fits their value. That matches the existing 0601 variable-uint
  behavior.
- A non-default width remains available via the existing
  `LocalSetBuilder::set(..., len)` override, and `Message::set` preserves a
  source item's width on edit, so decoding a minimal-width packet and
  re-encoding it unedited still round-trips byte-exact.
- `ItemDescriptor::fixed_len` semantics are settled: **default encode width**
  when `variable = true`, fixed width otherwise. (The `types.hpp` comment
  "meaningful iff !variable" was wrong and is corrected.)
- Minimal-width encoding stays unimplemented for all variable-uint items —
  a separate feature if byte-parity with the standard's worked examples is
  ever required.
