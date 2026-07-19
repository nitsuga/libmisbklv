// SPDX-License-Identifier: Apache-2.0
// gstreamer-backed MediaBackend factory (ADR 0013). The implementation
// (src/gst/gst_backend.cpp) is built only when gstreamer is available — the
// misbklv-gst target (fork 14). This header stays gstreamer-free.
#pragma once

#include <memory>

#include "misbklv/backend.hpp"

namespace misbklv {

std::unique_ptr<MediaBackend> make_gst_backend();

}  // namespace misbklv
