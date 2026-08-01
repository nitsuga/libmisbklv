---
type: Prior Art
title: akrutsinger/libklv
description: Namesake stub — C library for ST 0601.9 KLV encoding. Name collides with this project.
tags: [prior-art, c, st0601, namesake]
generated:
  by: claude/opus-5
  at: 2026-07-17T13:30:00Z
resource: https://github.com/akrutsinger/libklv
sources:
  - id: libklv-akrutsinger
    resource: https://github.com/akrutsinger/libklv
    title: akrutsinger/libklv (README)
---

# What

C library (no license, 4★, updated 2026-07) for ST 0601.**9** KLV encoding.
The README is a one-line stub ("Library for MISB ST0601.9 Key-Length-Value
metadata encoding"); no real implementation is surfaced from the README, so
there is little to evaluate for design.

# Relevant to libmisbklv

- **Name collision.** This existing repo is also called `libklv`. It is a
  near-empty stub, but the name is taken on GitHub. Worth resolving before
  packaging/publishing: rename our project, or disambiguate in the description
  and package metadata (e.g. "libmisbklv — MISB KLV read/write with gstreamer/ffmpeg
  demux") so the two are distinguishable. → candidate `type: Decision`.
- **Avoid:** no code to crib; **no license**; older 0601.9 revision.

# Relationships

Same domain ([0601](./st0601.md)); otherwise unrelated to our design.

# Citations

[1] [github.com/akrutsinger/libklv](https://github.com/akrutsinger/libklv) (README).
