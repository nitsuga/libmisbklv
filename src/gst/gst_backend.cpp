// SPDX-License-Identifier: Apache-2.0
// Public factory and delegation layer for the private gstreamer backend units.
#include "misbklv/gst_backend.hpp"

#include "gst_backend_internal.hpp"

namespace misbklv {
namespace detail {

std::optional<std::pair<std::string, int>> parse_host_port(std::string_view rest) {
  std::string_view host;
  std::string_view port_view;
  if (rest.starts_with('[')) {
    const auto close = rest.find(']');
    if (close == std::string_view::npos || close + 1 >= rest.size() || rest[close + 1] != ':')
      return std::nullopt;
    host = rest.substr(1, close - 1);
    port_view = rest.substr(close + 2);
  } else {
    const auto colon = rest.rfind(':');
    if (colon == std::string_view::npos) return std::nullopt;
    host = rest.substr(0, colon);
    if (host.find(':') != std::string_view::npos) return std::nullopt;
    port_view = rest.substr(colon + 1);
  }
  if (host.empty() || port_view.empty()) return std::nullopt;

  const std::string port_string(port_view);
  try {
    std::size_t parsed = 0;
    const int port = std::stoi(port_string, &parsed);
    if (parsed != port_string.size() || port <= 0 || port > 65535) return std::nullopt;
    return std::pair{std::string(host), port};
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace detail

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
