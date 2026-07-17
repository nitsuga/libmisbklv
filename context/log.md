# Knowledge Bundle Log

## 2026-07-17

* **Initialization**: Established OKF v0.1 bundle — `index.md`, `log.md`, `CONVENTIONS.md`.
* **Schema**: Seeded open `type` vocabulary (Standard Reference, Encoding Rule, KLV Item, Prior Art, Decision, Component) in `CONVENTIONS.md`.
* **Pointer**: Filled root `CLAUDE.md` as the auto-load entry into this bundle.
* **Ingest**: Added five `type: Standard Reference` concepts —
  [`st0107`](./st0107.md) (KLV core), [`st1201`](./st1201.md) (IMAP mapping),
  [`st0601`](./st0601.md) (UAS Datalink LS), [`st0903`](./st0903.md) (VMTI),
  [`st0604`](./st0604.md) (ES timestamps). Cross-linked dependencies;
  populated `index.md` Standard References section.
* **Ingest**: Added five `type: Prior Art` concepts —
  [`klvdata`](./prior-art-klvdata.md), [`klvp`](./prior-art-klvp.md),
  [`libmisb0601`](./prior-art-libmisb0601.md),
  [`gstklvplugin`](./prior-art-gstklvplugin.md),
  [`akrutsinger/libklv`](./prior-art-libklv-akrutsinger.md).
  Key takeaways: parser-vs-item-DB split (klvp), INI-driven tag registry +
  PMT `0x06`+`KLVA` signaling + element decomposition (gstklvplugin), the
  ffmpeg `-map data-re` demux idiom on our exact sample (klvdata). Flagged
  the `akrutsinger/libklv` name collision for a future Decision.
  Populated `index.md` Prior Art section.
* **Ingest**: Added [`data-samples`](./data-samples.md) (`type: Sample Data`) —
  characterized `Day Flight.mpg` / `Night Flight IR.mpg` via `ffprobe` + a TS/PMT/UL
  scan. Verified: ST 0601 UL key, BER long-form length, tag-2-first packet;
  KLV PID `0x1F1`, **`stream_type=0x06` + `KLVA`** (not `0x15`) — validates the
  gstklvplugin mux-signaling note against real samples.
* **Lint**: Promoted `Sample Data` from anticipated to current type in
  [`CONVENTIONS`](./CONVENTIONS.md) (first use). Added a `Sample Data` section
  to `index.md`.
* **Structure**: Added [`../planning/`](../planning/) (ROADMAP.md, PROGRESS.md)
  for transient planning/progress — distinct from this KB (evergreen) and
  `../docs/` (user-facing). Added a `# Decisions` section to
  [`CONVENTIONS`](./CONVENTIONS.md) with the `status:` vocabulary
  (proposed/accepted/superseded/deferred) and the open-fork → Decision
  lifecycle. Root `CLAUDE.md` now points at `planning/`.
* **Structure**: Created [`./decisions/`](./decisions/) subdir (with an ADR
  register, [`./decisions/index.md`](./decisions/index.md)) to group Decision
  concepts; root `index.md` Decisions section now points to it. Added a
  `# Subdirectories` policy to [`CONVENTIONS`](./CONVENTIONS.md): types stay
  flat by default, split only when noisy (~10+) or sub-structured (likely
  future split: `items/` for per-item 0601 concepts, deferred). Fork 7 in
  [`../planning/ROADMAP`](../planning/ROADMAP.md) narrowed to ADR
  format/numbering (location settled).
* **Process**: Added a `## Planning hygiene` directive to root `CLAUDE.md` —
  on a significant decision, write/update the ADR in `context/decisions/` and
  refresh ROADMAP (fork status + link) + PROGRESS; on implementing a
  significant change, refresh PROGRESS (and ROADMAP if a fork moves); routine
  ingests/lint stay in `log.md`. Ensures planning stays non-ephemeral.
