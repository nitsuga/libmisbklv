# Running the suite against another gstreamer

`run.sh` builds and tests this repo inside a container pinned to a distro's
gstreamer, so you can run the suite against a version other than your own.

```sh
tools/gst-container/run.sh                      # whole suite, ubuntu 24.04 (gst 1.24)
tools/gst-container/run.sh -R gst_video_insert  # ctest arguments pass through
UBUNTU=22.04 tools/gst-container/run.sh         # gst 1.20 instead
GST_DEBUG=2,aggregator:5 tools/gst-container/run.sh -R gst_video_insert
```

The image is built on first use and cached. The repo is mounted **read-only**;
the build lands in `build-container-<version>/` on the host, so rebuilds are
incremental and `.gitignore` already covers it.

## Status — `run.sh` is not yet verified end to end

**Proven:** the image, and building + running the suite inside it. That path was
used repeatedly on 2026-07-28 and is what found the two bugs below — 25/25 under
gstreamer 1.24.2, twice.

**Not proven:** `run.sh` itself. Two bugs in the wrapper were found and fixed
while writing it (a `-t` flag that fails without a terminal, and `[ -t 1 ] && …`
ending the script under `set -e`), and then the host's docker daemon became
unresponsive before a clean run completed. So the script is committed as a
starting point, not as something known to work. **Run it once and confirm before
trusting it.** If it misbehaves, this is the invocation that was actually used —
the script is only a wrapper around it:

```sh
docker build --network=host -t misbklv-dev:24.04 tools/gst-container
docker run --rm \
  -v "$PWD:/src:ro" -v "$PWD/build-container-24.04:/b" -e HOME=/tmp \
  misbklv-dev:24.04 bash -c '
    cmake -S /src -B /b -DCMAKE_BUILD_TYPE=Release >/dev/null &&
    cmake --build /b -j"$(nproc)" >/dev/null &&
    ctest --test-dir /b --output-on-failure --timeout 180'
```

## Why bother

Because gstreamer's behaviour differs between versions in ways that hide real
bugs, and CI was the only thing testing against a modern one. On 2026-07-28 this
container found two faults that a gstreamer 1.20 host could not reproduce at all:

- **A preroll deadlock.** Dropped streams went straight to a `fakesink`; a sink
  in `PAUSED` blocks its thread until `PLAYING`, and a demuxer feeds every stream
  from one thread, so the dropped stream blocked the video behind it and the
  pipeline never prerolled. It hung CI for six hours a run.
- **Silent loss of ST 0604 replacement.** gstreamer 1.22 added a parsed payload
  type for `user_data_unregistered`; the code only recognised the older
  *unhandled* form, so on 1.24 a source's own timestamps survived next to the
  generated ones. Nothing failed — the output was just wrong.

The second is the reason to keep reaching for this: it produced no error
anywhere, and only a test that decodes the output caught it.

Reproducing locally also turns an 11-minute CI round trip into about 90 seconds,
which is the difference between debugging and guessing.

## If the image build crawls

Almost certainly an MTU mismatch, not your connection. Docker's bridge defaults
to an MTU of 1500; if the host interface is lower — WSL2 defaults to **1420** —
every packet above the host MTU is silently dropped, and `apt` does not fail, it
crawls. Measured here: 24 minutes versus 73 seconds.

```sh
ip link | grep mtu        # host interface MTU
```

`run.sh` already builds with `--network=host`, which inherits the correct MTU and
is why it is there. A system-wide fix, if you want one, is `{"mtu": 1420}` in
`/etc/docker/daemon.json`.

## Keeping it honest

The package list in `Dockerfile` mirrors the `Install deps` step in
[`.github/workflows/ci.yml`](../../.github/workflows/ci.yml). If one changes and
the other does not, this stops predicting CI — which is most of its value.
