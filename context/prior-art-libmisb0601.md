---
type: Prior Art
title: n1tsu/libmisb0601
description: C encode/decode library for ST 0601.6; init→add_klv→finalize encode API.
tags: [prior-art, c, st0601]
generated:
  by: claude/opus-5
  at: 2026-07-17T13:30:00Z
resource: https://github.com/n1tsu/libmisb0601
sources:
  - id: libmisb0601
    resource: https://github.com/n1tsu/libmisb0601
    title: n1tsu/libmisb0601 (README)
---

# What

C library (no license, 3★, updated 2024-10) to encode/decode ST 0601.**6**
(an older revision than our 0601.19).

# Approach

- Encode: `initialize_packet()` → `add_klv(packet, FieldMap[MISSION], value)`
  → `finalize_packet()` (adds checksum + total length). Values are a
  `GenericValue` tagged union (`CHAR_P`, `FLOAT`, …). `FieldMap` constants
  name the tags.
- Decode: `unpack_misb(data, size, klvmap)` fills a `KLVMap` whose `KLVs[94]`
  array is indexed by tag; direct access via `klvmap[UNIX_TIME_STAMP]`.

# Relevant to libmisbklv

- **Crib:** the encode API shape (init → add → finalize, with a typed value
  union and a tag-name map) and the decode-to-a-tag-indexed-map shape are both
  reasonable starting templates for a C++ API.
- **Avoid:** the **fixed 94-element tag array** is 0601.6-era and not
  future-proof for 0601.19's ~143 items, nested sets, or multi-use items —
  don't replicate that assumption. **No license** — design reference only. C,
  not C++; dated revision.

# Relationships

Implements items of [0601](./st0601.md) (at revision .6).

# Citations

[1] [github.com/n1tsu/libmisb0601](https://github.com/n1tsu/libmisb0601) (README).
