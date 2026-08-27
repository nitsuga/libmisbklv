// SPDX-License-Identifier: Apache-2.0
// Multicast/broadcast knobs on `udp:` sinks (ADR 0031, part 1). Constructs the
// sink element via the internal make_sink seam and inspects the udpsink
// properties directly — no network traffic and no full pipeline, so the mapping
// from InsertConfig fields to udpsink ttl-mc / multicast-iface / loop /
// auto-multicast is pinned without being topology-dependent. Also checks that
// defaults reproduce today's exact behavior and that file:/srt: sinks ignore
// the knobs.
#include <cstdio>
#include <string>

#include <gst/gst.h>

#include "gst_backend_internal.hpp"
#include "misbklv/backend.hpp"

using namespace misbklv;
using misbklv::detail::make_sink;

static int failures = 0;

static void check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

// Read a udpsink property and print/verify it.
static void expect_udp_props(const char* spec, const InsertConfig& cfg, int want_ttl,
                             const char* want_iface, bool want_loop, bool want_auto_mc) {
  GstElement* s = make_sink(spec, cfg);
  std::printf("  udp '%s' -> sink=%s\n", spec, s ? "created" : "NULL");
  check(s != nullptr, "udp sink created");
  if (!s) return;
  check(GST_IS_ELEMENT(s) != FALSE, "returned a GstElement");

  gint ttl = -1;
  gchar* iface = nullptr;
  gboolean loop = FALSE, am = FALSE;
  g_object_get(s, "ttl-mc", &ttl, "multicast-iface", &iface, "loop", &loop, "auto-multicast", &am,
               nullptr);
  std::printf("    ttl-mc=%d multicast-iface=%s loop=%d auto-multicast=%d\n", ttl,
              iface ? iface : "(null)", static_cast<int>(loop), static_cast<int>(am));
  check(ttl == want_ttl, "ttl-mc matches");
  // multicast-iface unset reads back as null (udpsink default); a requested
  // iface comes back populated.
  if (want_iface == nullptr)
    check(iface == nullptr || std::string(iface) == "", "multicast-iface unset");
  else
    check(iface && std::string(iface) == want_iface, "multicast-iface matches");
  check(loop == (want_loop ? TRUE : FALSE), "loop matches");
  check(am == (want_auto_mc ? TRUE : FALSE), "auto-multicast matches");
  if (iface) g_free(iface);
  gst_object_unref(s);
}

int main() {
  gst_init(nullptr, nullptr);

  std::puts("== defaults (must match today's exact behavior) ==");
  {
    InsertConfig cfg;
    cfg.sink = "udp:127.0.0.1:5000";  // default udp_ttl_mcast=1, iface="", loop=true
    expect_udp_props("udp:127.0.0.1:5000", cfg, /*want_ttl=*/1,
                     /*want_iface=*/nullptr, /*want_loop=*/true,
                     /*want_auto_mc=*/true);
  }

  std::puts("== ttl override on a multicast group ==");
  {
    InsertConfig cfg;
    cfg.sink = "udp:239.1.1.1:5000";
    cfg.udp_ttl_mcast = 2;
    expect_udp_props("udp:239.1.1.1:5000", cfg, /*want_ttl=*/2,
                     /*want_iface=*/nullptr, /*want_loop=*/true,
                     /*want_auto_mc=*/true);
  }

  std::puts("== egress interface pinning ==");
  {
    InsertConfig cfg;
    cfg.sink = "udp:239.1.1.1:5000";
    cfg.udp_mcast_iface = "lo";
    expect_udp_props("udp:239.1.1.1:5000", cfg, /*want_ttl=*/1,
                     /*want_iface=*/"lo", /*want_loop=*/true,
                     /*want_auto_mc=*/true);
  }

  std::puts("== loop disabled ==");
  {
    InsertConfig cfg;
    cfg.sink = "udp:239.255.0.1:5000";
    cfg.udp_loop = false;
    expect_udp_props("udp:239.255.0.1:5000", cfg, /*want_ttl=*/1,
                     /*want_iface=*/nullptr, /*want_loop=*/false,
                     /*want_auto_mc=*/true);
  }

  std::puts("== bracketed IPv6 multicast with knobs ==");
  {
    InsertConfig cfg;
    cfg.sink = "udp:[ff02::1]:5000";
    cfg.udp_ttl_mcast = 3;
    cfg.udp_loop = false;
    expect_udp_props("udp:[ff02::1]:5000", cfg, /*want_ttl=*/3,
                     /*want_iface=*/nullptr, /*want_loop=*/false,
                     /*want_auto_mc=*/true);
  }

  std::puts("== out-of-range ttl is rejected (no silent no-op) ==");
  {
    InsertConfig cfg;
    cfg.sink = "udp:239.1.1.1:5000";
    cfg.udp_ttl_mcast = 256;  // udpsink range is 0..255
    GstElement* s = make_sink("udp:239.1.1.1:5000", cfg);
    check(s == nullptr, "ttl=256 rejected");
    if (s) gst_object_unref(s);
  }

  std::puts("== knobs ignored by file: and srt: sinks ==");
  {
    InsertConfig cfg;
    cfg.udp_ttl_mcast = 7;
    cfg.udp_mcast_iface = "lo";
    cfg.udp_loop = false;
    GstElement* f = make_sink("file:out.ts", cfg);
    check(f != nullptr && std::string(G_OBJECT_TYPE_NAME(f)) == "GstFileSink",
          "file sink is filesink, not udpsink");
    if (f) gst_object_unref(f);
    GstElement* st = make_sink("srt://127.0.0.1:5000", cfg);
    check(st != nullptr && std::string(G_OBJECT_TYPE_NAME(st)) != "GstUDPSink",
          "srt sink is not udpsink");
    if (st) gst_object_unref(st);
  }

  std::printf("\nUDP MULTICAST KNOBS: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
