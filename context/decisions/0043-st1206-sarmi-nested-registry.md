---
type: Decision
title: ST 1206 SAR Motion Imagery LS as a typed nested registry
decision_status: accepted
tags: [decision, registry, 1206, sarmi, nested-ls, phase-3]
generated:
  by: openai/gpt-5
  at: 2026-09-26T00:00:00Z
fork: 39
sources:
  - id: st1206
    resource: ../../references/ST1206.1.txt
    title: MISB ST 1206.1 §6.1.1, Table 1 — SAR Motion Imagery Metadata Local Set
---

# Context

ST 0601 Item 95 carries the ST 1206 SAR Motion Imagery (SARMI) Local Set as
opaque bytes. The standard's 28-item Table 1 uses existing scalar types except
for tag 22, Radar Cross Section Scale Factor Polynomial, which is an ST 1303
MDARRAY. ADR 0042 already added a bounded structural MDARRAY parser.

# Decision

Add `SARMI_1206` as a nested-only registry and change ST 0601 Item 95 to
`nested_ls` with child `sarmi_1206`. Do not add its standalone UL to packet
demux or `Message::create()` in this slice.

Map Table 1's uint8/uint16/uint32/uint64 fields to fixed-width `uint`, its
IMAPB fields to their stated ranges and widths, and tag 22 to `mdarray`.
Tags 2 (Ground Plane Squint Angle), 3 (Look Direction), and 28 (Document
Version) are mandatory: ST 1206-04/-05 require them in every SARMI Local Set.
The reference-frame tags 23-27 are conditional metadata for SAR
coherent-change products, not universally mandatory.

The implementation reuses the established registry/codegen routing: a
`RegistryId`, generator child mapping, generated header, `registry_for()` arm,
regeneration and CI drift entries, plus a routing and mandatory-field test.

# Consequences

Callers can descend from ST 0601 Item 95 using generated descriptors and parse
tag 22 with `mdarray::parse()`. No new parser, builder, or top-level dispatch
mechanism is added. A standalone SARMI packet can be considered later when a
producer requires it.

The generic parser validates the MDARRAY structure, not ST 1206's item-specific
two-dimensional/IMAPB/0-to-1e6 contract. The registry test documents that
expected shape and mapping; strict item-specific validation remains caller work.

# Citations

[1] ST 1206.1 §6.1.1, Table 1 and ST 1206-05 — item types and Document Version
requirement.[^st1206]
[2] [ADR 0042](./0042-st1601-mdarray-registry.md) — reusable ST 1303 MDARRAY
structural parsing.

[^st1206]: `references/ST1206.1.txt`, pages 5-6 and §6.4.1.
