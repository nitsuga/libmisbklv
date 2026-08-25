# references/ — source-of-truth inputs, append-only

External inputs this library is built on: the MISB standards it implements,
deposited as the publisher's PDF plus a faithful `.txt` extract for grep/Read.
They are the ground truth that [`context/`](../context/) synthesizes.

Currently — six MISB standards, all authored by the Motion Imagery Standards
Board (MISB), a body of the U.S. National Geospatial-Intelligence Agency:

| File | Standard | Published | Added |
|---|---|---|---|
| `ST0107.5` | KLV Metadata in Motion Imagery | 21 October 2021 | 2026-07-17 |
| `ST0601.19` | UAS Datalink Local Set | 02 March 2023 | 2026-07-17 |
| `ST0603.5` | MISP Time System and Timestamps | 5 October 2017 | 2026-07-27 |
| `ST0604.6` | Timestamps for Class 1/Class 2 Motion Imagery | 5 October 2017 | 2026-07-17 |
| `ST0903.6` | Video Moving Target Indicator Metadata | 21 October 2021 | 2026-07-17 |
| `ST1201.5` | Floating Point to Integer Mapping | 24 June 2021 | 2026-07-17 |

Each is present twice: `NAME.pdf`, the publisher's document, tracked in
[git-lfs](https://git-lfs.com/) per `.gitattributes`; and `NAME.txt`, a text
extract of the same document kept in regular git so it is greppable and
diffable. The two carry the same content in different containers — a decision
about one is a decision about both.

Published by MISB at <https://nsgreg.nga.mil/misb.jsp>, with an older document
tree at `gwg.nga.mil/misb/docs/standards/`.

## Terms

MISB standards are works of the U.S. Federal Government, prepared by a board of
the NGA as part of its official duties. Under [17 U.S.C.
§ 105](https://www.law.cornell.edu/uscode/text/17/105) copyright protection is
not available for such works, which places them in the public domain in the
United States. The documents deposited here carry no copyright notice, no
distribution statement, and no license grant of any kind — consistent with that
status rather than in tension with it.

Two things this record does **not** claim, because neither has been
established:

- **Not confirmed from the publisher.** NGA's own endpoints were unreachable
  when this was written on 2026-08-25 (`nsgreg.nga.mil` did not respond;
  `gwg.nga.mil` returned HTTP 403), so the basis above is the statute plus a
  third-party republication of the same series — Wikimedia Commons hosts MISB
  ST 0601.8 under `PD-USGov`, sourced from `gwg.nga.mil`. Nobody at MISB has
  told this project anything.
- **U.S. only.** § 105 withholds copyright domestically; it does not prevent
  the U.S. Government from asserting copyright abroad. This is the ordinary
  PD-USGov caveat, not something specific to MISB.

Neither point blocked publishing this repository, and both belong on the record
rather than in a commit message.

Note that a MISB standard *cites* other bodies' standards — SMPTE ST 336 among
them — without reproducing them. Those remain their publishers' work and are
not redistributed here.

## Append-only, not read-only

A directed ingest may *add* a source here; nothing already here is edited,
reformatted, or deleted. Each deposit is immutable — the directory still grows.

- Put the raw source here exactly as it came. If it needs correcting or
  interpreting, that is a `context/` concept citing it, not an edit here.
- Cite a standard by name and section (`ST 0601 §6.3`), never by extract line
  number — line numbers shift across revisions.
- Check the terms *before* depositing, not after, and record them here.
