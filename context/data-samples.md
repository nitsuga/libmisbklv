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
sample server; the three `.ts` files matched the public QGISFMV sample archive
(`QGISFMV_Samples.7z`), which is linked from that plugin's quick start with no
stated license or attribution. The FFmpeg directory likewise supplied no license
grant. Public availability was not treated as permission to redistribute.

The three `.ts` files shared a distribution channel but **not** a rightsholder —
each was identified from its own ST 0601 items, not from a recorded download:

| File | Identifying items | Reading |
|---|---|---|
| `Cheyenne.ts` | Mission ID `ESRI_Metadata_Collect`, tail `N97826`, platform `C208B`; 2012-09-19; sensor track 41.13 N, 104.79 W | Esri. Matches the sample shipped with the ArcGIS Full Motion Video tutorial (a semi-trailer on a highway near Cheyenne, Wyoming). Esri sample data carries no redistribution grant. |
| `falls.ts` | Platform `L3 Wescam - MX Turret`, sensors `EOW - DL` / `EOW_HD`; 2016-04-13; sensor track 47.55 N, 121.88 W (Snoqualmie Falls, Washington) | **A separate rightsholder from the other two.** The PMT also carried vendor-private streams tagged `WESCAM`, `VIDEO_BM_0`, `ARSX`, `JSONCMD`, `KLV_SYNC`, `KLV_ASYNC` — OEM recorder output, not a scrubbed public sample. No source URL or terms were ever located; the highest-risk file of the five. |
| `klv_metadata_test_sync.ts` | Mission ID `1234567-89`, platform `Predator`, sensor `EO Nose`; 2017-12-12 | Synthetic metadata. Those strings are the literal example values printed in [ST 0601](./st0601.md) Items 10 and 11, so the KLV track was injector-generated over a short carrier clip. The metadata is very unlikely to be protectable; the carrier video's origin was never established. |

This table is the durable record of *why* the corpus was removed, and is not
reconstructible once the media is gone — hence its retention here.

The extracted `.klv` fixtures derived from that corpus were removed with it.
Historical ADRs and the durable log retain the probe results because they explain
backend behavior and design decisions; those references describe past evidence,
not files currently shipped by the project.

# Current fixtures

Regression inputs are project-authored, deterministic, and covered by
Apache-2.0. [`../test/fixtures/generate_synthetic_fixtures.py`](../test/fixtures/generate_synthetic_fixtures.py)
regenerates the committed KLV and MPEG-TS cases used by tests, including:

- basic and single-packet ST 0601 streams;
- comprehensive values, special values, variable widths, and a multi-byte BER-OID tag;
- timed and untimed `stream_type 0x06` PES; and
- timed `stream_type 0x15` metadata access units;
- a project-owned synthetic video carrier for passthrough and remux tests.

The committed fixtures let a clean checkout build and test without Python or
network access. Their optional generator uses only the Python 3.11+ standard
library and invented values; it is both recipe and provenance, and CI rejects
drift between the committed bytes and fresh regeneration.

# Citations

[1] [`../data/README.md`](../data/README.md) — current developer-media policy.
[2] [`../test/fixtures/generate_synthetic_fixtures.py`](../test/fixtures/generate_synthetic_fixtures.py) — current fixture generator and provenance.
[3] [`0028`](./decisions/0028-hermetic-synthetic-fixtures.md) — the accepted migration decision.
