---
type: Prior Art
title: jimcavoy/klvp
description: C++ KLV parser/encoder for ST 0601 + ST 0102; splits parser from a Local Set database.
tags: [prior-art, cpp, st0601, st0102, stanag4609]
timestamp: 2026-07-17T13:30:00Z
resource: https://github.com/jimcavoy/klvp
---

# What

C++ KLV parser/encoder (no license declared, 5★, updated 2026-07). ST 0601 +
ST 0102, STANAG 4609. CMake + vcpkg. Exports a CMake package for importing
into other projects.

# Approach

Two static libraries — the split is the interesting part:

- **`klvp`** — the KLV parser and encoder (the mechanics: BER, TLV).
- **`ldsdp`** — the Local Dataset (LDS) database (the 0601/0102 item
  definitions: tags, lengths, decode semantics).

Plus `klv2xml`, an example app that reads a KLV stream and emits XML.

# Relevant to libklv

- **Crib:** the parser-vs-item-database split is a clean separation worth
  adopting — keep KLV mechanics (BER-OID tags, BER length, TLV walk) isolated
  from the per-standard item registry. The `klv2xml` example is a good model
  for a debug/inspection CLI. CMake package export pattern.
- **Avoid:** **no license** — design reference only, do not copy code; vcpkg
  is a heavyweight dependency; covers 0601+0102, not [0903](/st0903.md) or
  [0604](/st0604.md).
- Closest in spirit to libklv (C++), so the best architectural foil.

# Relationships

Profile of [0107](/st0107.md); items of [0601](/st0601.md) + ST 0102.

# Citations

[1] [github.com/jimcavoy/klvp](https://github.com/jimcavoy/klvp) (README).
