# libmisbklv

C++ library to read and write MISB KLV metadata — ST 0601 (UAS Datalink Local
Set) + ST 0903 (VMTI) — from/to MPEG-TS containers via
[GStreamer](https://gstreamer.freedesktop.org/) (file or stream; real-time
insertion via `appsrc`). ST 0604 (ES-layer timestamps) and an ffmpeg backend
are deferred — see [ADR 0008](context/decisions/0008-media-backend-gstreamer.md)
and [ADR 0009](context/decisions/0009-st0604-deferred.md).

## Status

Early design phase. Architecture is captured as ADRs in
[`context/decisions/`](context/decisions/); see
[`planning/ROADMAP.md`](planning/ROADMAP.md) and
[`planning/PROGRESS.md`](planning/PROGRESS.md) for the plan and current status.

## License

Apache-2.0 — see [`LICENSE`](LICENSE).
