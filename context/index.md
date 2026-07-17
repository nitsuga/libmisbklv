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

## Encoding Rules

_(`type: Encoding Rule` — KLV mechanics: BER-OID tags, BER length, value encoding, ZLI/Report-on-Change.)_

## KLV Items

_(`type: KLV Item` — individual 0601/0903 items: tag, length, units, decode method.)_

## Prior Art

_(`type: Prior Art` — existing KLV libraries/plugins analyzed as design input.)_

## Decisions

_(`type: Decision` — architecture decision records: gstreamer vs ffmpeg, C++ standard, 0604 scope.)_

## Components

_(`type: Component` — libklv modules: KLV core, gstreamer backend, ffmpeg backend.)_
