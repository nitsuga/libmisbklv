---
okf_version: "0.1"
---

# libklv Knowledge Bundle

Agent-facing knowledge base for libklv, in
[OKF](https://github.com/GoogleCloudPlatform/knowledge-catalog/blob/main/okf/SPEC.md)
v0.1. The agent owns this directory; humans curate sources and direct the work.

Read [`CONVENTIONS.md`](./CONVENTIONS.md) before ingesting or editing.

## Bundle guide

* [Conventions](./CONVENTIONS.md) — frontmatter, types, linking, ingest/query/lint rules.

## Standard References

_(`type: Standard Reference` — per-MISB-standard synthesis, citing the immutable PDFs in `../references/`.)_

* [ST 0107.5](./st0107.md) — KLV core mechanics (the keystone; restates paywalled SMPTE 336).
* [ST 1201.5](./st1201.md) — IMAPA/IMAPB float↔int mapping + special values.
* [ST 0601.19](./st0601.md) — UAS Datalink Local Set (~143 items); the core FMV metadata.
* [ST 0903.6](./st0903.md) — VMTI metadata; nests under 0601.
* [ST 0604.6](./st0604.md) — Precision/Nano/commercial timestamps in the video ES (SEI/user_data).

## Encoding Rules

_(`type: Encoding Rule` — KLV mechanics: BER-OID tags, BER length, value encoding, ZLI/Report-on-Change.)_

## KLV Items

_(`type: KLV Item` — individual 0601/0903 items: tag, length, units, decode method.)_

## Prior Art

_(`type: Prior Art` — existing KLV libraries/plugins analyzed as design input.)_

* [paretech/klvdata](./prior-art-klvdata.md) — Python, MIT; 0601+0102; punts demux to ffmpeg (uses our `Day Flight.mpg`).
* [jimcavoy/klvp](./prior-art-klvp.md) — C++, no license; parser-vs-item-DB split; closest architectural foil.
* [n1tsu/libmisb0601](./prior-art-libmisb0601.md) — C, no license; encode/decode 0601.6; fixed 94-tag array (anti-pattern).
* [mkassimi98/gstklvplugin](./prior-art-gstklvplugin.md) — GStreamer plugin, AGPL-3.0; enc/dec/inject/PMT-rewrite; INI registry; the gstreamer-path reference.
* [akrutsinger/libklv](./prior-art-libklv-akrutsinger.md) — C stub, 0601.9; **name collides with this project**.

## Sample Data

_(`type: Sample Data` — characterized input assets / test vectors.)_

* [data/ — KLV MPEG-TS samples](./data-samples.md) — Day Flight & Night Flight IR; verified ST 0601 in MPEG-TS, `0x06`+`KLVA` signaling.

## Decisions

_(`type: Decision` — architecture decision records: gstreamer vs ffmpeg, C++ standard, 0604 scope.)_

## Components

_(`type: Component` — libklv modules: KLV core, gstreamer backend, ffmpeg backend.)_
