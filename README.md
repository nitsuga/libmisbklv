# libmisbklv

C++ library to read and write MISB KLV metadata — ST 0601 (UAS Datalink Local
Set), ST 0903 (VMTI), ST 0604 (ES-layer timestamps) — from/to MPEG-TS
containers via [GStreamer](https://gstreamer.freedesktop.org/) (file or
stream; real-time insertion via `appsrc`). An ffmpeg backend is optional and
deferred — see [ADR 0008](context/decisions/0008-media-backend-gstreamer.md).

## Status

Early design phase. Architecture is captured as ADRs in
[`context/decisions/`](context/decisions/); see
[`planning/ROADMAP.md`](planning/ROADMAP.md) and
[`planning/PROGRESS.md`](planning/PROGRESS.md) for the plan and current status.

## License

Apache-2.0 — see [`LICENSE`](LICENSE).
