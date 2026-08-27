// SPDX-License-Identifier: Apache-2.0
// Public factory and delegation layer for the private gstreamer backend units.
#include "misbklv/gst_backend.hpp"

#include "gst_backend_internal.hpp"

namespace misbklv {
namespace {

class GstBackend : public MediaBackend {
 public:
  Result<std::monostate> extract(std::string_view source, const PacketHandler& on_packet,
                                 std::stop_token stop = {}, ExtractOptions options = {}) override {
    gst_init(nullptr, nullptr);
    return detail::extract(source, on_packet, stop, options);
  }

  Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig& cfg) override {
    gst_init(nullptr, nullptr);
    return detail::open_insert(cfg);
  }
};

}  // namespace

std::unique_ptr<MediaBackend> make_gst_backend() {
  return std::make_unique<GstBackend>();
}

}  // namespace misbklv
