---
type: Sample Data
title: Historical media corpus and fixture migration
description: The external media corpus was removed; project-owned synthetic fixtures now provide hermetic regression coverage.
tags: [sample-data, mpegts, st0601, test-vector, provenance]
generated:
  by: claude/opus-5
  at: 2026-08-08T00:00:00Z
resource: ../data
sources:
  - id: ffmpeg-samples
    resource: https://samples.ffmpeg.org/MPEG2/mpegts-klv/
    title: Historical FFmpeg sample MPEG-TS KLV files
  - id: qgisfmv-samples
    resource: https://drive.google.com/file/d/137JaQwx5kVwhdcrxwTCSgxqBbaOjW9be/view
    title: Historical QGISFMV sample collection
---

# Status

The five third-party media files formerly distributed in [`../data/`](../data/)
were removed on 2026-08-08. Their public hosting locations did not provide a
redistribution license, and the repository's Apache-2.0 license never covered
third-party media. The current policy and migration decision are in [ADR
0028](./decisions/0028-hermetic-synthetic-fixtures.md).

`data/` remains as an ignored workspace for developer-provided media. It is not
a source of test inputs, and no external video is needed to configure, build, or
run the standard test suite.

# Historical corpus

The removed files were: `Day Flight.mpg`, `Night Flight IR.mpg`, `Cheyenne.ts`,
`falls.ts`, and `klv_metadata_test_sync.ts`. The first two came from the FFmpeg
sample server; the three `.ts` files matched the public QGISFMV sample archive.
The archive's historical documentation associated the QGISFMV samples with Esri,
while the FFmpeg directory supplied no license grant. Public availability was
not treated as permission to redistribute.

The extracted `.klv` fixtures derived from that corpus were removed with it.
Historical ADRs and the durable log retain the probe results because they explain
backend behavior and design decisions; those references describe past evidence,
not files currently shipped by the project.

# Current fixtures

Regression inputs are project-authored, deterministic, and covered by
Apache-2.0. [`../test/fixtures/generate_synthetic_fixtures.py`](../test/fixtures/generate_synthetic_fixtures.py)
generates the KLV and MPEG-TS cases used by tests, including:

- basic and single-packet ST 0601 streams;
- comprehensive values, special values, variable widths, and a multi-byte BER-OID tag;
- timed and untimed `stream_type 0x06` PES; and
- timed `stream_type 0x15` metadata access units; and
- a project-owned synthetic video carrier for passthrough and remux tests.

The generator uses only the Python 3.11+ standard library and invented values.
It is a recipe as well as provenance for the generated fixtures, so a clean
checkout can recreate them without downloading media from an external site.

# Citations

[1] [`../data/README.md`](../data/README.md) — current developer-media policy.
[2] [`../test/fixtures/generate_synthetic_fixtures.py`](../test/fixtures/generate_synthetic_fixtures.py) — current fixture generator and provenance.
[3] [`0028`](./decisions/0028-hermetic-synthetic-fixtures.md) — the accepted migration decision.
