# syntax=docker/dockerfile:1.7
# CI image for libmisbklv. Deps are baked into a layer so jobs never run
# apt at runtime -- that is what made concurrent jobs race on the dpkg lock.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# BuildKit cache mounts keep the apt archives and lists between image
# builds, so adding one package re-downloads only that package. docker-clean
# is removed because it deletes the very archives we are caching.
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    rm -f /etc/apt/apt.conf.d/docker-clean \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ccache git python3 ca-certificates \
      libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
      libgstreamer-plugins-bad1.0-dev \
      gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
      gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly

# CMake reads these from the environment, so the compiler cache applies
# without a CMakeLists change. CCACHE_DIR is a mounted volume in ci.yml.
ENV CCACHE_DIR=/ccache \
    CCACHE_MAXSIZE=5G \
    CMAKE_C_COMPILER_LAUNCHER=ccache \
    CMAKE_CXX_COMPILER_LAUNCHER=ccache
