# SPDX-License-Identifier: Apache-2.0
# Build+test environment pinned to a distro's gstreamer, so the suite can be run
# against a version other than the developer's. See run.sh and README.md.
#
# The package list mirrors .github/workflows/ci.yml's "Install deps" step — if
# one changes, change the other, or the container stops predicting CI.
ARG UBUNTU=24.04
FROM ubuntu:${UBUNTU}

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake g++ make pkg-config ca-certificates \
      libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
      libgstreamer-plugins-bad1.0-dev \
      gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
      gstreamer1.0-plugins-bad \
 && rm -rf /var/lib/apt/lists/*
