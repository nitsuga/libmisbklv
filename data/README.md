# Developer-provided media

This directory is intentionally kept in the source tree, but it does not ship
with project test media. Developers may place their own MPEG-TS, MPEG, or other
video files here while investigating an integration or reproducing a report.
Files in this directory are ignored by Git; do not add third-party media or
other files whose redistribution terms are unclear.

The regular build and test suite does not depend on this directory. Its
hermetic KLV and MPEG-TS inputs live under [`../test/fixtures/`](../test/fixtures/)
and are generated from project-authored values by
[`generate_synthetic_fixtures.py`](../test/fixtures/generate_synthetic_fixtures.py).
Those fixtures are covered by the repository's Apache-2.0 license and are the
right place for deterministic regression inputs.

If a locally supplied video is needed for an optional experiment, reference its
absolute or workspace-local path from that experiment instead of adding it to
the repository. The removed historical samples and their provenance are
documented in [`../context/data-samples.md`](../context/data-samples.md).
