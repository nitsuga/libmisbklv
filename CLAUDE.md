# libklv

C++ library to read and write MISB KLV data — ST 0601 (UAS Datalink Local
Set), ST 0903 (VMTI), ST 0604 (ES-layer timestamps) — from/to MPEG-TS
containers, via gstreamer or ffmpeg, file or stream.

## Agent knowledge base

The project's working knowledge lives in `context/` as an
[OKF](https://github.com/GoogleCloudPlatform/knowledge-catalog/blob/main/okf/SPEC.md)
v0.1 bundle.

- Read [`context/index.md`](context/index.md) first — the bundle catalog.
- [`context/CONVENTIONS.md`](context/CONVENTIONS.md) — frontmatter, type
  vocabulary, ingest/query/lint rules. Read before editing `context/`.
- `context/` is agent-owned. `references/` is immutable — read, never modify.

## Repo layout

- `references/` — MISB standards (PDF + `.txt` extract). Source of truth.
- `context/` — agent knowledge bundle (maintain this).
- `docs/` — human-facing guides (terse).
- `data/` — sample MPEG-TS test vectors (Day Flight, Night Flight IR).
