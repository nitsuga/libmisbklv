---
type: Prior Art
title: paretech/klvdata
description: Python KLV parser/constructor for ST 0601 + ST 0102; punts MPEG-TS demux to ffmpeg/gstreamer.
tags: [prior-art, python, st0601, st0102, stanag4609]
timestamp: 2026-07-17T13:30:00Z
resource: https://github.com/paretech/klvdata
---

# What

Python library (MIT, 110★, updated 2026-05) to parse and construct KLV. Covers
ST 0601 (UAS Datalink LS) and ST 0102 (Security LS), STANAG 4609 TS. No
external deps. `pip install klvdata`.

# Approach

- `klvdata.StreamParser(bytes)` yields packets; `packet.structure()` prints a
  nested tree. Each item is a typed class (`PrecisionTimeStamp`,
  `SensorLatitude`, `UASLocalMetadataSet`, …). Unknown tags surface as
  `UnknownElement` — the [0107](./st0107.md) skip-unknown / future-proof
  pattern.
- **Deliberately does not demux.** The README states klvdata alone cannot
  extract KLV from an MPEG-TS; it expects an external demuxer. Its own quick
  start runs `ffmpeg -i Day\ Flight.mpg -map data-re -codec copy -f data -`
  to pull the KLV elementary stream, then pipes bytes into `StreamParser`.

# Relevant to libmisbklv

- **Crib:** the per-item class names are a ready 0601 item vocabulary; the
  `StreamParser`-yields-packets API shape; the `UnknownElement` future-proofing.
- **The ffmpeg idiom is load-bearing** and works on our exact sample
  (see [`data-samples`](./data-samples.md); `data-re` is the KLV/data PID).
  Validates the [ffmpeg](#) extraction path; see
  [gstklvplugin](./prior-art-gstklvplugin.md) for the gstreamer equivalent.
- **Avoid:** Python (we are C++); it externalizes demux — which is precisely
  libmisbklv's differentiator (integrated, configurable gstreamer/ffmpeg demux).

# Relationships

Profile of [0107](./st0107.md); implements items of [0601](./st0601.md) + ST 0102.

# Citations

[1] [github.com/paretech/klvdata](https://github.com/paretech/klvdata) (README).
