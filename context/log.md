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
