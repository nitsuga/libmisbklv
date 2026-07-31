#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build and run the test suite against a gstreamer other than this machine's.
#
#   tools/gst-container/run.sh                          # whole suite, gst 1.24
#   tools/gst-container/run.sh -R gst_video_insert      # ctest args pass through
#   UBUNTU=22.04 tools/gst-container/run.sh             # gst 1.20 instead
#   GST_DEBUG=2,aggregator:5 tools/gst-container/run.sh -R gst_video_insert
#
# Why this exists: gstreamer behavior differs enough between versions to hide
# real bugs. Two were found this way that a 1.20 host could not see at all — a
# preroll deadlock that hung CI for hours a run, and, behind it, ST 0604 SEI
# replacement silently doing nothing on 1.22+. See context/log.md, 2026-07-28.
set -euo pipefail

UBUNTU="${UBUNTU:-24.04}"
IMAGE="misbklv-dev:${UBUNTU}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Kept out of the repo by .gitignore's /build-*/ rule, and per-version so
# switching UBUNTU does not force a full rebuild each time.
BUILD_DIR="${REPO}/build-container-${UBUNTU}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "building $IMAGE (first run only)..."
  # --network=host is not optional on a host whose interface MTU is below the
  # docker bridge's 1500 — WSL2 defaults to 1420. The mismatch black-holes any
  # packet over the host MTU, and apt does not fail, it crawls: 24 minutes here
  # versus 73 seconds with this flag. Check with `ip link | grep mtu`.
  docker build --network=host \
    --build-arg "UBUNTU=${UBUNTU}" \
    -t "$IMAGE" "$(dirname "${BASH_SOURCE[0]}")"
fi

mkdir -p "$BUILD_DIR"

# The repo is mounted read-only: this runs the tests, it does not edit sources.
# The build directory is a persistent host mount so rebuilds are incremental.
# HOME is set because gstreamer wants somewhere to write its plugin registry.
# -t only when there is a terminal, so this works from a script or CI too.
# An `if` rather than `[ -t 1 ] && ...`, which returns non-zero without a TTY and
# would end the script here under `set -e`.
TTY_FLAG=()
if [ -t 1 ]; then TTY_FLAG=(-t); fi

exec docker run --rm "${TTY_FLAG[@]}" \
  -v "${REPO}:/src:ro" \
  -v "${BUILD_DIR}:/b" \
  -e HOME=/tmp \
  -e "GST_DEBUG=${GST_DEBUG:-}" \
  -e GST_DEBUG_NO_COLOR=1 \
  "$IMAGE" bash -c '
    set -e
    echo "gstreamer $(pkg-config --modversion gstreamer-1.0) on ubuntu '"${UBUNTU}"'"
    cmake -S /src -B /b -DCMAKE_BUILD_TYPE=Release > /b/configure.log 2>&1 \
      || { tail -30 /b/configure.log; exit 1; }
    cmake --build /b -j"$(nproc)" > /b/build.log 2>&1 \
      || { tail -30 /b/build.log; exit 1; }
    # --timeout so a hang is a named failure rather than a wait. The suite takes
    # a few seconds; anything near 180 is stuck, not slow.
    ctest --test-dir /b --output-on-failure --timeout 180 '"$*"'
  '
