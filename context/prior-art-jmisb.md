---
type: Prior Art
title: WestRidgeSystems/jmisb
description: Mature MIT-licensed Java MISB library; broadest peer (0601/0903/1201/0102/…). Per-item-class architecture (the approach we rejected); mine for correctness + test vectors, not design.
tags: [prior-art, java, st0601, st0903, st1201, st0102, imap, test-vectors]
generated:
  by: claude/opus-5
  at: 2026-07-18T00:00:00Z
resource: https://github.com/WestRidgeSystems/jmisb
sources:
  - resource: https://github.com/WestRidgeSystems/jmisb
    title: WestRidgeSystems/jmisb — api/src/main/java/org/jmisb/api/klv/
---

# What

Open-source Java MISB library (**MIT** license). The most complete peer found:
implements our whole v1 scope and ~20 more standards — `st0601`, `st0903`,
`st1201`, `st0102`, `st0603`, `st0805/0806/0808`, `st1204`, `st1206`, `st1303`,
`st1403`, `st190x`, `eg0104`. Active, well-tested.

# Approach

A **generic KLV core** + a **class per item** (the klvdata camp):

- Core (`api/.../klv/`): `Ber`/`BerDecoder`/`BerEncoder`, `KlvParser`,
  `LdsParser` (Local Set) vs `UdsParser` (Universal Data Set), `UniversalLabel`,
  `ArrayBuilder`, `IKlvValue`/`INestedKlvValue`, `CrcCcitt`. Maps almost 1:1 to
  our BER/[`packet`](./st0107.md) mechanics (ST 0107 covers BER-OID + BER length).
- Per-standard packages hold **hand-written classes per item** (e.g. an
  `st0601/` class per tag). This is the anti-pattern
  [`0006`](./decisions/0006-tag-registry.md) rejected in favor of a data-driven
  `constexpr` registry — so jmisb is an *architectural foil*, not a template.
- `st1201/`: `FpEncoder` (IMAP forward+reverse, ~25 KB), `OutOfRangeBehaviour`,
  `ValueMappingKind` (IMAPA/IMAPB), `DecodeResult`.

# Relevant to libmisbklv

- **Crib (highest value):**
  - **Test vectors.** Its JUnit suite has byte-array golden vectors for
    0601/0903/1201 — independent ground truth to replace our hand-authored
    fixtures ([`data-samples`](./data-samples.md); M3/M5/M6 fixtures are all
    hand-built). Strongest use of this repo.
  - **IMAP correctness.** `st1201/FpEncoder` + `OutOfRangeBehaviour` cross-check
    our [`st1201`](./st1201.md) codec — the Zoffset truncation edge (fixed in M6)
    and the IMAP `BELOW/ABOVE_MINIMUM` special-value semantics we have **not**
    yet implemented (we detect explicit special patterns only).
  - **Concepts we deferred:** **UDS** (Universal Data Set, UL-keyed — jmisb
    splits `UdsParser` from `LdsParser`; we do Local Sets only) and the **Array
    type** (0903 §9.1.2 `ArrayBuilder`; we did Series in M6, not Arrays).
  - **ST 0603 time** (`st0603`: `ST0603TimeStamp`, `TimeStatus`) — the timestamp
    seam shared with our 0601 item-2 and with the deferred
    [`0604`](./decisions/0009-st0604-deferred.md) (0604 payloads are 0603-based).
- **Does NOT cover:** **ST 0604** (ES-layer timestamps) — no SEI/NAL/H.26x in the
  repo; jmisb is KLV-only, confirming 0604 is a separate video-ES subsystem (see
  [`0009`](./decisions/0009-st0604-deferred.md)).
  - **Breadth/semantics reference** as we grow the registry (item ranges, units,
    special values), a second source alongside the standards.
- **Avoid:**
  - **Architecture** — class-per-item + object-per-item allocation, no
    zero-copy; opposite of our data-driven registry + `std::span`
    ([`0005`](./decisions/0005-klv-core-data-model.md),
    [`0006`](./decisions/0006-tag-registry.md)). Do not port structure.
  - **License hygiene** — MIT permits reuse *with attribution*. Reference freely;
    if a test vector or snippet is lifted verbatim, preserve the MIT notice and
    cite. Facts read off the standard (byte layouts) are not owned by jmisb.

# Relationships

Profile of [0107](./st0107.md); overlaps our [0601](./st0601.md),
[0903](./st0903.md), [1201](./st1201.md). Peer to [klvdata](./prior-art-klvdata.md)
(also per-item classes) and [klvp](./prior-art-klvp.md) (C++ parser/DB split);
jmisb is the most complete of the three.

# Citations

[1] [github.com/WestRidgeSystems/jmisb](https://github.com/WestRidgeSystems/jmisb)
    — MIT; `api/src/main/java/org/jmisb/api/klv/` package tree.
