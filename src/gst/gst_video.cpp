// SPDX-License-Identifier: Apache-2.0
#include "gst_backend_internal.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gst/base/gsttypefindhelper.h>

#include "misbklv/codec.hpp"
#include "misbklv/packet.hpp"
#include "misbklv/registries.hpp"

namespace misbklv::detail {
namespace {

inline constexpr std::chrono::seconds kVideoPadTimeout{10};
inline constexpr std::chrono::seconds kLivePadTimeout{5};
inline constexpr std::uint64_t kPtsMatchToleranceNs = 200'000'000;
// A direct-space miss must exceed this before the shifted space is even
// tried: far above any legitimate KLV-vs-video timing skew (the match
// tolerance is two orders smaller), far below the headroom shift itself.
inline constexpr std::uint64_t kPtsShiftDetectThresholdNs = 10ULL * GST_SECOND;
inline constexpr std::uint8_t kTimeStatusReserved = 0b0001'1111;
inline constexpr std::uint8_t kTimeStatusLockUnknown = 0b1000'0000;
inline constexpr std::uint8_t kTimeStatusDiscontinuity = 0b0100'0000;
inline constexpr std::uint8_t kTimeStatusReverse = 0b0010'0000;
inline constexpr std::uint8_t kTimeStatusBase = kTimeStatusLockUnknown | kTimeStatusReserved;
inline constexpr std::int64_t kTimeLinearToleranceUs = 50'000;

bool caps_are_video(GstCaps* caps) {
  if (!caps || gst_caps_is_empty(caps)) return false;
  const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  return name && std::string_view(name).starts_with("video/");
}

std::string caps_media_type(GstCaps* caps) {
  if (!caps || gst_caps_is_empty(caps)) return {};
  const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  return name ? std::string(name) : std::string();
}

enum class CapsCodec { Unknown, H264, KnownNonH264 };

CapsCodec caps_codec(GstCaps* caps) {
  if (!caps || gst_caps_is_empty(caps) || gst_caps_is_any(caps)) return CapsCodec::Unknown;
  GstStructure* s = gst_caps_get_structure(caps, 0);
  if (!s) return CapsCodec::Unknown;
  if (gst_structure_has_name(s, "video/x-h264")) return CapsCodec::H264;
  const gchar* n = gst_structure_get_name(s);
  if (n && std::string_view(n).starts_with("video/")) return CapsCodec::KnownNonH264;
  return CapsCodec::Unknown;
}

void latch_codec_from_caps(VideoCtx& ctx, GstCaps* caps) {
  const CapsCodec c = caps_codec(caps);
  if (c == CapsCodec::H264)
    ctx.codec_latch.store(CodecLatch::IsH264, std::memory_order_relaxed);
  else if (c == CapsCodec::KnownNonH264)
    ctx.codec_latch.store(CodecLatch::NotH264, std::memory_order_relaxed);
}

const char* demuxer_for_media_type(const std::string& type) {
  // This used to be parsebin's job. parsebin decides a stream is fully parsed
  // by asking whether any decoder in the registry accepts its caps — a decoder
  // it never instantiates. Normal systems hide this dependency because one is
  // installed; minimal bundles do not, so parsebin never reaches final caps and
  // reports "no suitable plugins found" for a stream it already parsed. The
  // explicit table avoids requiring an H.264 decoder merely to not decode H.264
  // (ADR 0025).
  if (type == "video/quicktime") return "qtdemux";
  if (type == "video/mpegts") return "tsdemux";
  if (type == "video/x-matroska") return "matroskademux";
  return nullptr;
}

std::string sniff_container(const std::string& path) {
  // The known path lets us settle the container before a pipeline exists, so
  // filesrc ! demuxer is built in one piece rather than a dynamic have-type
  // branch racing preroll. 16 KiB is generous; a low-confidence answer is
  // refused instead of half-plugging an unsupported file.
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return {};
  std::vector<guint8> head(16384);
  const size_t n = std::fread(head.data(), 1, head.size(), file);
  std::fclose(file);
  if (n == 0) return {};
  GstTypeFindProbability probability = GST_TYPE_FIND_NONE;
  GstCaps* caps = gst_type_find_helper_for_data(nullptr, head.data(), n, &probability);
  if (!caps) return {};
  std::string type;
  if (probability >= GST_TYPE_FIND_POSSIBLE) type = caps_media_type(caps);
  gst_caps_unref(caps);
  return type;
}

const char* parser_for_media_type(const std::string& type) {
  // H.26x from MP4 needs conversion from avc/hvc1 container form to byte-stream
  // for mpegtsmux. MPEG-1/2 needs framing too: parsebin previously inserted a
  // parser for every stream, while a bare TS demuxer hands MPEG video through
  // unparsed and the muxer fails only at finish(). Parsers are idempotent, so a
  // listed parser is harmless when a demuxer happened to parse already.
  if (type == "video/x-h264") return "h264parse";
  if (type == "video/x-h265") return "h265parse";
  if (type == "video/mpeg") return "mpegvideoparse";
  if (type == "video/x-h263") return "h263parse";
  if (type == "video/x-vp9") return "vp9parse";
  return nullptr;
}

const char* mux_caps_for_media_type(const std::string& type) {
  // The video pad is reserved while the pipeline is NULL and only linked after
  // PAUSED, by which point it is activated and will not renegotiate on link
  // (the sink template's fixed caps win). A capsfilter with these caps —
  // exactly what mpegtsmux's sink template accepts — makes that link safe
  // regardless of the parser's negotiation state: the capsfilter's src pad has
  // no current caps until data flows, so the link is checked against its ANY
  // template, and the fixed caps force the byte-stream / parsed form the muxer
  // accepts (ADR 0020 § stream order; the avc current-caps NOFORMAT case).
  if (type == "video/x-h264") return "video/x-h264, stream-format=byte-stream";
  if (type == "video/x-h265") return "video/x-h265, stream-format=byte-stream";
  if (type == "video/mpeg") return "video/mpeg, systemstream=false, parsed=true";
  return nullptr;
}

std::vector<std::byte> generate_0604_sei_payload(std::uint64_t timestamp_microsec,
                                                 std::uint8_t time_status) {
  // ST 0604.6 §7: User Unregistered type 5, UUID "MISPmicrosectime", status,
  // and the modified timestamp with its prescribed 0xFF separators.
  std::vector<std::byte> payload;
  payload.push_back(std::byte{5});
  payload.push_back(std::byte{28});
  const std::uint8_t uuid[16] = {'M', 'I', 'S', 'P', 'm', 'i', 'c', 'r',
                                 'o', 's', 'e', 'c', 't', 'i', 'm', 'e'};
  for (int i = 0; i < 16; ++i) payload.push_back(std::byte{uuid[i]});
  payload.push_back(std::byte{time_status});
  const auto ts_byte = [timestamp_microsec](unsigned i) {
    return std::byte{static_cast<std::uint8_t>((timestamp_microsec >> (8 * (7 - i))) & 0xFF)};
  };
  payload.push_back(ts_byte(0));
  payload.push_back(ts_byte(1));
  payload.push_back(std::byte{0xFF});
  payload.push_back(ts_byte(2));
  payload.push_back(ts_byte(3));
  payload.push_back(std::byte{0xFF});
  payload.push_back(ts_byte(4));
  payload.push_back(ts_byte(5));
  payload.push_back(std::byte{0xFF});
  payload.push_back(ts_byte(6));
  payload.push_back(ts_byte(7));
  return payload;
}

std::vector<std::byte> build_0604_sei_nal(const SensorTime& t) {
  std::vector<std::byte> nal{std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x06}};
  const auto payload = generate_0604_sei_payload(t.timestamp_us, t.status);
  nal.insert(nal.end(), payload.begin(), payload.end());
  nal.push_back(std::byte{0x80});
  return nal;
}

std::uint8_t sensor_time_status(std::int64_t delta_ts_us, std::int64_t delta_pts_us) {
  if (delta_ts_us < 0) return kTimeStatusBase | kTimeStatusDiscontinuity | kTimeStatusReverse;
  if (std::llabs(delta_ts_us - delta_pts_us) > kTimeLinearToleranceUs)
    return kTimeStatusBase | kTimeStatusDiscontinuity;
  return kTimeStatusBase;
}

bool nal_is_vcl(guint type) {
  return (type >= GST_H264_NAL_SLICE && type <= GST_H264_NAL_SLICE_IDR) ||
         type == GST_H264_NAL_SLICE_AUX || type == GST_H264_NAL_SLICE_EXT ||
         type == GST_H264_NAL_SLICE_DEPTH;
}

bool sei_message_is_replaced(const GstH264SEIMessage& msg) {
  // Version-dependent parsing matters here: before 1.22 user-data-unregistered
  // reaches codecparsers as raw type 5, while newer versions expose a typed
  // payload. Missing either case silently leaves the source SEI beside ours.
  if (msg.payloadType == GST_H264_SEI_PIC_TIMING) return true;
  static const char kId[] = "MISPmicrosectime";
  constexpr guint kIdLen = 16;
#if GST_CHECK_VERSION(1, 22, 0)
  if (msg.payloadType == GST_H264_SEI_USER_DATA_UNREGISTERED)
    return std::memcmp(msg.payload.user_data_unregistered.uuid, kId, kIdLen) == 0;
#endif
  if (msg.payloadType != GST_H264_SEI_UNHANDLED_PAYLOAD) return false;
  const auto& raw = msg.payload.unhandled_payload;
  if (raw.payloadType != 5 || !raw.data || raw.size < kIdLen) return false;
  return std::memcmp(raw.data, kId, kIdLen) == 0;
}

bool sei_nal_is_replaced(GstH264NalParser* parser, GstH264NalUnit* nalu) {
  // Drop a whole SEI NAL only when every message is ours to replace. A mixed
  // NAL may contain buffering/recovery data beside a source timestamp, and
  // taking that bystander would be lossy.
  GArray* messages = nullptr;
  const GstH264ParserResult res = gst_h264_parser_parse_sei(parser, nalu, &messages);
  bool all = false;
  if (messages) {
    if (res == GST_H264_PARSER_OK && messages->len > 0) {
      all = true;
      for (guint i = 0; i < messages->len && all; ++i)
        all = sei_message_is_replaced(g_array_index(messages, GstH264SEIMessage, i));
    }
    g_array_free(messages, TRUE);
  }
  return all;
}

GstPadProbeReturn on_h264_buffer_inject_sei(GstPad* pad, GstPadProbeInfo* info, gpointer user);

GstPadProbeReturn on_sei_event_probe(GstPad*, GstPadProbeInfo* info, gpointer user) {
  auto* ctx = static_cast<VideoCtx*>(user);
  GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
  if (!event) return GST_PAD_PROBE_OK;
  if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
    GstCaps* caps = nullptr;
    gst_event_parse_caps(event, &caps);
    latch_codec_from_caps(*ctx, caps);
  } else if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
    const GstSegment* segment = nullptr;
    gst_event_parse_segment(event, &segment);
    if (segment && segment->format == GST_FORMAT_TIME) {
      std::lock_guard<std::mutex> lock(ctx->timestamp_mu);
      gst_segment_copy_into(segment, &ctx->video_segment);
      ctx->have_video_segment = true;
      // Re-detect after a discontinuity or renegotiation: the new segment may
      // describe a different timeline (or even a different encoder branch).
      ctx->use_segment_timeline = false;
    }
  }
  return GST_PAD_PROBE_OK;
}

void attach_generate_probes(GstPad* pad, VideoCtx* ctx) {
  // Record each probe so teardown can sever it before ctx is freed (issue #57).
  const gulong event_id =
      gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, on_sei_event_probe, ctx, nullptr);
  ctx->register_probe(pad, event_id);
  const gulong buf_id =
      gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_h264_buffer_inject_sei, ctx, nullptr);
  ctx->register_probe(pad, buf_id);
}

GstPadProbeReturn on_video_buffer_delivered(GstPad*, GstPadProbeInfo*, gpointer user) {
  static_cast<VideoCtx*>(user)->delivered_buffer.store(true, std::memory_order_release);
  return GST_PAD_PROBE_OK;
}

void attach_delivery_probe(VideoCtx* ctx) {
  if (!ctx || !ctx->reserved_video_pad) return;
  const gulong id = gst_pad_add_probe(ctx->reserved_video_pad, GST_PAD_PROBE_TYPE_BUFFER,
                                      on_video_buffer_delivered, ctx, nullptr);
  ctx->register_probe(ctx->reserved_video_pad, id);
}

GstPadProbeReturn on_h264_buffer_inject_sei(GstPad* pad, GstPadProbeInfo* info, gpointer user) {
  auto* ctx = static_cast<VideoCtx*>(user);
  if (!ctx->generate_sei || !ctx->h264_parser) return GST_PAD_PROBE_OK;
  const CodecLatch latch = ctx->codec_latch.load(std::memory_order_relaxed);
  if (latch == CodecLatch::NotH264) return GST_PAD_PROBE_OK;
  if (latch == CodecLatch::Unknown) {
    // No CAPS event seen yet — fall back to per-buffer query to preserve the
    // defense against un-negotiated pads.
    if (pad) {
      GstCaps* caps = gst_pad_get_current_caps(pad);
      if (!caps) caps = gst_pad_query_caps(pad, nullptr);
      const bool is_h264 = caps_codec(caps) == CapsCodec::H264;
      if (caps) gst_caps_unref(caps);
      if (!is_h264) return GST_PAD_PROBE_OK;
    }
  }
  GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buffer || !GST_BUFFER_PTS_IS_VALID(buffer)) return GST_PAD_PROBE_OK;
  // Backward-only matching prevents a frame receiving a later KLV timestamp;
  // no match within tolerance writes no replacement. An unmatched frame is
  // still scanned: Generate makes KLV the sole timestamp authority, so source
  // ST 0604 must not survive intermittently based on provenance.
  //
  // The caller's KLV PTS and the video branch's raw buffer PTS are expected to
  // share one timeline. Encoders that opt into gst_video_encoder_set_min_pts()
  // (x264enc, avenc_h264 — since GStreamer 1.6) shift their output onto a
  // 1000-hour minimum so the first DTS stays non-negative. The adjustment is
  // min_pts - first_input_pts, not a fixed 1000 hours, and is reflected in the
  // downstream segment. On a miss too large to be ordinary timing skew, retry
  // at the segment-derived running time; once it matches, latch that space for
  // the segment. A direct hit never latches, so pipelines whose timelines
  // already agree behave exactly as before (ADR 0033).
  const std::uint64_t pts_ns = GST_BUFFER_PTS(buffer);
  SensorTime sensor_time;
  bool matched = false;
  {
    std::lock_guard<std::mutex> lock(ctx->timestamp_mu);
    const auto match_at = [&](std::uint64_t key_ns) {
      auto it = ctx->pts_to_sensor_timestamp.upper_bound(key_ns);
      if (it != ctx->pts_to_sensor_timestamp.begin()) {
        --it;
        if (key_ns - it->first <= kPtsMatchToleranceNs) {
          sensor_time = it->second;
          return true;
        }
      }
      return false;
    };
    const auto segment_running_time = [&]() -> GstClockTime {
      if (!ctx->have_video_segment) return GST_CLOCK_TIME_NONE;
      return gst_segment_to_running_time(&ctx->video_segment, GST_FORMAT_TIME, pts_ns);
    };
    std::uint64_t key_ns = pts_ns;
    bool have_key = true;
    if (ctx->use_segment_timeline) {
      const GstClockTime running_time = segment_running_time();
      if (GST_CLOCK_TIME_IS_VALID(running_time)) {
        key_ns = running_time;
        matched = match_at(key_ns);
      } else {
        // Do not evict on a raw, known-wrong encoded timestamp while waiting
        // for a usable segment after a discontinuity.
        have_key = false;
      }
    } else if (match_at(pts_ns)) {
      matched = true;
    } else {
      // Direct space missed. Only entertain the shifted space when the miss
      // is far beyond any legitimate tolerance skew (two orders above it) —
      // otherwise this is an ordinary unmatched frame.
      auto nearest = ctx->pts_to_sensor_timestamp.upper_bound(pts_ns);
      const std::uint64_t dist = nearest == ctx->pts_to_sensor_timestamp.begin()
                                     ? UINT64_MAX
                                     : pts_ns - std::prev(nearest)->first;
      if (dist > kPtsShiftDetectThresholdNs) {
        const GstClockTime running_time = segment_running_time();
        if (GST_CLOCK_TIME_IS_VALID(running_time)) {
          key_ns = running_time;
          matched = match_at(key_ns);
          ctx->use_segment_timeline = matched;
        } else {
          // The raw key is already known to be in the wrong space. Preserve
          // the map until a usable segment or direct-space frame arrives.
          have_key = false;
        }
      }
    }
    // Consumer-side eviction: entries older than key_ns - tolerance can never
    // match a future frame under monotonic video PTS, so they are safe to
    // discard. This bounds the map by the actual KLV-vs-video lead, not by a
    // wall-clock guess. If Generate is on and KLV is pushed far ahead with
    // video never arriving, entries accumulate until consumed — inherent, as
    // the data may still be needed. (key_ns, not raw pts_ns: on a shift-
    // detected branch the map lives on the caller's unshifted timeline.)
    if (have_key && key_ns >= kPtsMatchToleranceNs) {
      const std::uint64_t threshold = key_ns - kPtsMatchToleranceNs;
      auto eit = ctx->pts_to_sensor_timestamp.begin();
      while (eit != ctx->pts_to_sensor_timestamp.end() && eit->first < threshold)
        eit = ctx->pts_to_sensor_timestamp.erase(eit);
    }
  }
  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;
  // Collect Picture Timing/ST 0604 NALs to strip as [start,end) offsets, and
  // the first VCL NAL before which to inject. They are offsets, never pointers:
  // mapping a multi-memory GstBuffer again can return a different merged
  // allocation, invalidating pointers from a prior map.
  std::vector<std::pair<gsize, gsize>> strip;
  gsize insert_at = 0;
  bool have_insert = false;
  GstH264NalUnit nalu;
  guint offset = 0;
  while (offset < map.size) {
    const GstH264ParserResult res =
        gst_h264_parser_identify_nalu(ctx->h264_parser, map.data, offset, map.size, &nalu);
    if (res != GST_H264_PARSER_OK && res != GST_H264_PARSER_NO_NAL_END) break;
    if (nalu.type == GST_H264_NAL_SEI) {
      if (sei_nal_is_replaced(ctx->h264_parser, &nalu))
        strip.emplace_back(nalu.sc_offset, nalu.offset + nalu.size);
    } else if (nal_is_vcl(nalu.type) && !have_insert) {
      insert_at = nalu.sc_offset;
      have_insert = true;
    }
    if (res == GST_H264_PARSER_NO_NAL_END) break;
    offset = nalu.offset + nalu.size;
  }
  const bool inject = matched && have_insert;
  if (!inject && strip.empty()) {
    gst_buffer_unmap(buffer, &map);
    return GST_PAD_PROBE_OK;
  }
  const auto sei_nal = inject ? build_0604_sei_nal(sensor_time) : std::vector<std::byte>{};
  gsize stripped = 0;
  for (const auto& [s, e] : strip) stripped += e - s;
  const gsize new_size = map.size - stripped + sei_nal.size();
  if (new_size == 0) {
    gst_buffer_unmap(buffer, &map);
    return GST_PAD_PROBE_OK;
  }
  GstBuffer* new_buffer = gst_buffer_new_allocate(nullptr, new_size, nullptr);
  if (!new_buffer) {
    gst_buffer_unmap(buffer, &map);
    return GST_PAD_PROBE_OK;
  }
  GstMapInfo new_map;
  if (!gst_buffer_map(new_buffer, &new_map, GST_MAP_WRITE)) {
    gst_buffer_unref(new_buffer);
    gst_buffer_unmap(buffer, &map);
    return GST_PAD_PROBE_OK;
  }
  guint8* dst = new_map.data;
  gsize pos = 0;
  std::size_t si = 0;
  bool inserted = !inject;
  while (pos < map.size) {
    if (!inserted && pos == insert_at) {
      std::memcpy(dst, sei_nal.data(), sei_nal.size());
      dst += sei_nal.size();
      inserted = true;
    }
    if (si < strip.size() && pos == strip[si].first) {
      pos = strip[si].second;
      ++si;
      continue;
    }
    gsize run_end = map.size;
    if (!inserted && insert_at > pos) run_end = std::min(run_end, insert_at);
    if (si < strip.size() && strip[si].first > pos) run_end = std::min(run_end, strip[si].first);
    std::memcpy(dst, map.data + pos, run_end - pos);
    dst += run_end - pos;
    pos = run_end;
  }
  const gsize written = static_cast<gsize>(dst - new_map.data);
  gst_buffer_unmap(new_buffer, &new_map);
  gst_buffer_unmap(buffer, &map);
  // If size arithmetic and copying disagree, pass the original through rather
  // than emit a buffer with an uninitialized tail.
  if (written != new_size || !inserted) {
    gst_buffer_unref(new_buffer);
    return GST_PAD_PROBE_OK;
  }
  gst_buffer_copy_into(new_buffer, buffer,
                       static_cast<GstBufferCopyFlags>(GST_BUFFER_COPY_METADATA), 0, -1);
  GST_PAD_PROBE_INFO_DATA(info) = new_buffer;
  gst_buffer_unref(buffer);
  return GST_PAD_PROBE_OK;
}

void drop_pad_to_fakesink(GstPad* pad, VideoCtx* ctx) {
  // A demuxer pushes every stream from one streaming thread. A plain sink in
  // PAUSED prerolls and blocks that thread until PLAYING; then video queued
  // behind it never reaches the muxer, the muxer never prerolls, and the sink
  // never unblocks. The leaky downstream queue gives discarded pads a separate
  // thread and prevents that 1.24 CI preroll deadlock.
  GstElement* queue = gst_element_factory_make("queue", nullptr);
  GstElement* fakesink = gst_element_factory_make("fakesink", nullptr);
  if (!queue || !fakesink) {
    if (queue) gst_object_unref(queue);
    if (fakesink) gst_object_unref(fakesink);
    return;
  }
  g_object_set(queue, "leaky", 2, "max-size-buffers", 5, "max-size-bytes", 0, "max-size-time",
               G_GUINT64_CONSTANT(0), nullptr);
  g_object_set(fakesink, "async", FALSE, "sync", FALSE, nullptr);
  gst_bin_add_many(GST_BIN(ctx->pipeline), queue, fakesink, nullptr);
  gst_element_sync_state_with_parent(queue);
  gst_element_sync_state_with_parent(fakesink);
  if (!gst_element_link(queue, fakesink)) {
    g_warning("misbklv: failed to link the drop queue to its sink");
    return;
  }
  if (GstPad* sinkpad = gst_element_get_static_pad(queue, "sink")) {
    const GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    if (ret != GST_PAD_LINK_OK)
      g_warning("misbklv: failed to link unused pad to drop queue: %d", ret);
    gst_object_unref(sinkpad);
  }
}

void on_video_pad_added(GstElement*, GstPad* pad, gpointer user) {
  auto* ctx = static_cast<VideoCtx*>(user);
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) caps = gst_pad_query_caps(pad, nullptr);
  const bool video = caps_are_video(caps);
  const std::string media_type = caps_media_type(caps);
  if (caps) gst_caps_unref(caps);
  if (video && ctx->generate_sei && media_type != "video/x-h264") {
    std::lock_guard<std::mutex> lk(ctx->mu);
    if (!ctx->linked) {
      g_warning("misbklv: Sei0604::Generate needs H.264 video; source carries %s",
                media_type.empty() ? "an unknown codec" : media_type.c_str());
      drop_pad_to_fakesink(pad, ctx);
      ctx->cv.notify_all();
      return;
    }
  }
  if (!video) {
    // Audio, subtitles, and source-side KLV are all linked to the leaky drop
    // branch; leaving a demuxer pad unlinked posts a not-linked pipeline error.
    drop_pad_to_fakesink(pad, ctx);
    return;
  }
  std::lock_guard<std::mutex> lk(ctx->mu);
  if (ctx->linked) {
    ++ctx->ignored_video_pads;
    drop_pad_to_fakesink(pad, ctx);
    return;
  }
  const char* parser_name = parser_for_media_type(media_type);
  if (!parser_name) {
    // No parser needed: link the demuxer pad straight onto the reserved pad.
    // The reserved pad is already activated, so the link is checked against the
    // source's current caps; for these codecs the demuxer's caps already match
    // the muxer's template, so this holds. (A codec the muxer does not accept
    // fails here rather than at finish(), which is the honest error.)
    if (ctx->reserved_video_pad && gst_pad_link(pad, ctx->reserved_video_pad) == GST_PAD_LINK_OK)
      ctx->linked = true;
    else
      g_warning("misbklv: failed to link %s pad to muxer", media_type.c_str());
    ctx->cv.notify_all();
    return;
  }
  GstElement* parse = gst_element_factory_make(parser_name, nullptr);
  if (!parse) {
    g_warning("misbklv: failed to create %s element", parser_name);
    ctx->cv.notify_all();
    return;
  }
  if (ctx->generate_sei) g_object_set(parse, "config-interval", -1, nullptr);
  gst_bin_add(GST_BIN(ctx->pipeline), parse);
  gst_element_sync_state_with_parent(parse);
  GstPad* parse_sink = gst_element_get_static_pad(parse, "sink");
  if (!parse_sink) {
    g_warning("misbklv: failed to get %s sink pad", parser_name);
    ctx->cv.notify_all();
    return;
  }
  GstPadLinkReturn ret = gst_pad_link(pad, parse_sink);
  gst_object_unref(parse_sink);
  if (ret != GST_PAD_LINK_OK) {
    // gst_pad_get_parent_element() returns a full reference; hold it in a local
    // so the error path can name it and then drop it (GST_ELEMENT_PARENT does
    // not add a ref). Leaving it inline leaked the demuxer on every link failure.
    GstElement* demux_el = gst_pad_get_parent_element(pad);
    g_warning("misbklv: failed to link video pad to %s: %d (demuxer parent: %s, parser parent: %s)",
              parser_name, ret, demux_el ? GST_ELEMENT_NAME(demux_el) : "(unknown)",
              GST_ELEMENT_NAME(GST_ELEMENT_PARENT(parse)));
    if (demux_el) gst_object_unref(demux_el);
    ctx->cv.notify_all();
    return;
  }
  GstPad* parse_src = gst_element_get_static_pad(parse, "src");
  if (!parse_src) {
    g_warning("misbklv: failed to get %s src pad", parser_name);
    ctx->cv.notify_all();
    return;
  }
  // The video muxer pad was reserved while the pipeline was NULL so it takes
  // the lower ES PID and is announced first in the PMT (ADR 0020 § stream
  // order). By now it is activated, so it will not renegotiate on link — an
  // avc current-caps source would fail with NOFORMAT. Routing the stream
  // through a capsfilter whose src has no current caps (template ANY) makes
  // the link safe; its fixed caps then force the byte-stream form the muxer
  // accepts. Codecs outside the capsfilter table link the parser's src
  // directly (their current caps already match the muxer template).
  bool linked = false;
  if (const char* mux_caps = mux_caps_for_media_type(media_type)) {
    GstElement* capsfilter = gst_element_factory_make("capsfilter", nullptr);
    if (!capsfilter) {
      g_warning("misbklv: failed to create capsfilter for %s", media_type.c_str());
    } else {
      GstCaps* fcaps = gst_caps_from_string(mux_caps);
      g_object_set(capsfilter, "caps", fcaps, nullptr);
      gst_caps_unref(fcaps);
      gst_bin_add(GST_BIN(ctx->pipeline), capsfilter);
      gst_element_sync_state_with_parent(capsfilter);
      if (gst_element_link(parse, capsfilter)) {
        GstPad* cf_src = gst_element_get_static_pad(capsfilter, "src");
        if (cf_src) {
          linked = ctx->reserved_video_pad &&
                   gst_pad_link(cf_src, ctx->reserved_video_pad) == GST_PAD_LINK_OK;
          gst_object_unref(cf_src);
        }
      } else {
        g_warning("misbklv: failed to link %s to capsfilter", parser_name);
      }
    }
  } else {
    linked = ctx->reserved_video_pad &&
             gst_pad_link(parse_src, ctx->reserved_video_pad) == GST_PAD_LINK_OK;
  }
  if (!linked) {
    g_warning("misbklv: failed to link %s to the reserved muxer pad", parser_name);
    gst_object_unref(parse_src);
    ctx->cv.notify_all();
    return;
  }
  ctx->linked = true;
  if (ctx->generate_sei) {
    attach_generate_probes(parse_src, ctx);
    // Prime latch from the already-known H.264 demuxer caps so the buffer probe
    // does not stay Unknown when no further CAPS event arrives.
    ctx->codec_latch.store(CodecLatch::IsH264, std::memory_order_relaxed);
  }
  gst_object_unref(parse_src);
  ctx->cv.notify_all();
}

void on_video_no_more_pads(GstElement*, gpointer user) {
  auto* ctx = static_cast<VideoCtx*>(user);
  {
    std::lock_guard<std::mutex> lk(ctx->mu);
    ctx->no_more_pads = true;
  }
  ctx->cv.notify_all();
}

void on_rtspsrc_pad_added(GstElement*, GstPad* pad, gpointer user) {
  auto* ctx = static_cast<VideoCtx*>(user);
  // The server answered and described a stream, whatever we end up doing with
  // this pad. Distinguishes an unusable source from an absent one (ADR 0036).
  ctx->saw_any_pad.store(true, std::memory_order_release);
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) caps = gst_pad_query_caps(pad, nullptr);
  const std::string media_type = caps ? caps_media_type(caps) : std::string();
  std::string encoding;
  if (caps && !gst_caps_is_empty(caps)) {
    GstStructure* s = gst_caps_get_structure(caps, 0);
    if (gst_structure_has_name(s, "application/x-rtp")) {
      const gchar* enc = gst_structure_get_string(s, "encoding-name");
      if (enc) encoding = enc;
    }
  }
  // Decide if this pad is video we care about. RTSP audio pads are dropped.
  bool is_video_pad = false;
  const char* depay_name = nullptr;
  const char* parser_name = nullptr;
  std::string video_type;  // for mux caps lookup (video/x-h264 etc.)
  if (media_type == "application/x-rtp") {
    std::string enc_low = encoding;
    std::transform(enc_low.begin(), enc_low.end(), enc_low.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (enc_low == "h264") {
      depay_name = "rtph264depay";
      parser_name = "h264parse";
      video_type = "video/x-h264";
      is_video_pad = true;
    } else if (enc_low == "h265" || enc_low == "h264" || enc_low == "hevc") {
      // Normalize: H265 may arrive as H265 or HEVC.
      if (enc_low == "h265" || enc_low == "hevc") {
        depay_name = "rtph265depay";
        parser_name = "h265parse";
        video_type = "video/x-h265";
        is_video_pad = true;
      }
    }
  } else if (media_type == "video/x-h264") {
    parser_name = "h264parse";
    video_type = "video/x-h264";
    is_video_pad = true;
  } else if (media_type == "video/x-h265") {
    parser_name = "h265parse";
    video_type = "video/x-h265";
    is_video_pad = true;
  }
  if (caps) gst_caps_unref(caps);

  if (!is_video_pad) {
    // Unknown or non-video (audio, application) — drop to avoid not-linked.
    // If caps were unknown video type, this also ends as Unsupported via timeout
    // (no pad ever links), but we still drop the pad to keep the pipeline sane.
    drop_pad_to_fakesink(pad, ctx);
    if (!media_type.empty() && media_type.rfind("video/", 0) == 0) {
      std::lock_guard<std::mutex> lk(ctx->mu);
      // No linked video yet and this video caps is not H264/H265 -> future will be Unsupported
      // but we can't signal error synchronously; the 5s watch will handle it.
      g_warning("misbklv: RTSP video caps %s not supported", media_type.c_str());
    } else if (media_type == "application/x-rtp" && !encoding.empty()) {
      g_warning("misbklv: RTSP encoding %s not supported", encoding.c_str());
    }
    return;
  }

  // If Generate requested but codec is not H.264, we must not silently carry it;
  // the file branch handles this in on_video_pad_added, the RTSP path does same.
  if (ctx->generate_sei && video_type != "video/x-h264") {
    std::lock_guard<std::mutex> lk(ctx->mu);
    if (!ctx->linked) {
      g_warning("misbklv: Sei0604::Generate needs H.264 video; RTSP source carries %s",
                video_type.c_str());
      drop_pad_to_fakesink(pad, ctx);
      ctx->cv.notify_all();
      return;
    }
  }

  {
    std::lock_guard<std::mutex> lk(ctx->mu);
    if (ctx->linked) {
      ++ctx->ignored_video_pads;
      drop_pad_to_fakesink(pad, ctx);
      return;
    }
  }

  GstElement* depay = nullptr;
  GstElement* parse = nullptr;
  GstElement* queue = nullptr;
  GstElement* capsfilter = nullptr;

  if (depay_name) {
    depay = gst_element_factory_make(depay_name, nullptr);
    if (!depay) {
      g_warning("misbklv: failed to create %s", depay_name);
      drop_pad_to_fakesink(pad, ctx);
      std::lock_guard<std::mutex> lk(ctx->mu);
      ctx->cv.notify_all();
      return;
    }
  }
  if (parser_name) {
    parse = gst_element_factory_make(parser_name, nullptr);
    if (!parse) {
      g_warning("misbklv: failed to create %s", parser_name);
      if (depay) gst_object_unref(depay);
      drop_pad_to_fakesink(pad, ctx);
      std::lock_guard<std::mutex> lk(ctx->mu);
      ctx->cv.notify_all();
      return;
    }
    if (ctx->generate_sei) g_object_set(parse, "config-interval", -1, nullptr);
  }
  queue = gst_element_factory_make("queue", nullptr);
  if (!queue) {
    g_warning("misbklv: failed to create queue for RTSP branch");
    if (depay) gst_object_unref(depay);
    if (parse) gst_object_unref(parse);
    drop_pad_to_fakesink(pad, ctx);
    std::lock_guard<std::mutex> lk(ctx->mu);
    ctx->cv.notify_all();
    return;
  }

  // Capsfilter for the muxer byte-stream requirement, if applicable.
  const char* mux_caps_str = mux_caps_for_media_type(video_type);
  if (mux_caps_str) {
    capsfilter = gst_element_factory_make("capsfilter", nullptr);
    if (!capsfilter) {
      g_warning("misbklv: failed to create capsfilter for %s", video_type.c_str());
    } else {
      GstCaps* fcaps = gst_caps_from_string(mux_caps_str);
      g_object_set(capsfilter, "caps", fcaps, nullptr);
      gst_caps_unref(fcaps);
    }
  }

  // Add all to pipeline
  if (depay) gst_bin_add(GST_BIN(ctx->pipeline), depay);
  if (parse) gst_bin_add(GST_BIN(ctx->pipeline), parse);
  gst_bin_add(GST_BIN(ctx->pipeline), queue);
  if (capsfilter) gst_bin_add(GST_BIN(ctx->pipeline), capsfilter);
  if (depay) gst_element_sync_state_with_parent(depay);
  if (parse) gst_element_sync_state_with_parent(parse);
  gst_element_sync_state_with_parent(queue);
  if (capsfilter) gst_element_sync_state_with_parent(capsfilter);

  // Link pad -> first element
  GstElement* first = depay ? depay : (parse ? parse : queue);
  GstPad* first_sink = gst_element_get_static_pad(first, "sink");
  if (!first_sink) {
    g_warning("misbklv: failed to get sink pad for RTSP chain");
    {
      std::lock_guard<std::mutex> lk(ctx->mu);
      ctx->cv.notify_all();
    }
    return;
  }
  GstPadLinkReturn ret = gst_pad_link(pad, first_sink);
  gst_object_unref(first_sink);
  if (ret != GST_PAD_LINK_OK) {
    g_warning("misbklv: failed to link RTSP pad to %s: %d",
              first ? GST_ELEMENT_NAME(first) : "queue", ret);
    std::lock_guard<std::mutex> lk(ctx->mu);
    ctx->cv.notify_all();
    return;
  }

  // Link internal chain
  bool chain_ok = true;
  if (depay && parse) chain_ok = gst_element_link(depay, parse);
  if (!chain_ok) {
    g_warning("misbklv: failed to link %s to %s", depay_name ? depay_name : "",
              parser_name ? parser_name : "");
    std::lock_guard<std::mutex> lk(ctx->mu);
    ctx->cv.notify_all();
    return;
  }
  GstElement* after_parse = parse ? parse : depay;
  if (after_parse) {
    if (!gst_element_link(after_parse, queue)) {
      g_warning("misbklv: failed to link %s to queue", GST_ELEMENT_NAME(after_parse));
      std::lock_guard<std::mutex> lk(ctx->mu);
      ctx->cv.notify_all();
      return;
    }
  }
  // Queue -> capsfilter or directly to mux
  GstElement* before_mux = queue;
  if (capsfilter) {
    if (!gst_element_link(queue, capsfilter)) {
      g_warning("misbklv: failed to link queue to capsfilter");
      std::lock_guard<std::mutex> lk(ctx->mu);
      ctx->cv.notify_all();
      return;
    }
    before_mux = capsfilter;
  }

  GstPad* srcpad = gst_element_get_static_pad(before_mux, "src");
  if (!srcpad) {
    g_warning("misbklv: failed to get src pad for RTSP chain before mux");
    std::lock_guard<std::mutex> lk(ctx->mu);
    ctx->cv.notify_all();
    return;
  }
  bool linked =
      ctx->reserved_video_pad && gst_pad_link(srcpad, ctx->reserved_video_pad) == GST_PAD_LINK_OK;
  GstPad* probe_pad = nullptr;
  if (linked && parse) {
    // Probe on parser src for SEI injection if needed. Use parser src pad before capsfilter if
    // present.
    GstPad* parse_src = gst_element_get_static_pad(parse, "src");
    if (parse_src) {
      probe_pad = parse_src;  // will unref after probe add
    }
  } else if (linked) {
    probe_pad = srcpad;
    gst_object_ref(probe_pad);
  }
  gst_object_unref(srcpad);
  if (!linked) {
    g_warning("misbklv: failed to link RTSP chain to reserved muxer pad");
    if (probe_pad) gst_object_unref(probe_pad);
    std::lock_guard<std::mutex> lk(ctx->mu);
    ctx->cv.notify_all();
    return;
  }
  {
    std::lock_guard<std::mutex> lk(ctx->mu);
    ctx->linked = true;
  }
  if (ctx->generate_sei && probe_pad) {
    attach_generate_probes(probe_pad, ctx);
    // Prime latch from the already-known H.264 RTSP caps.
    ctx->codec_latch.store(CodecLatch::IsH264, std::memory_order_relaxed);
  }
  if (probe_pad) gst_object_unref(probe_pad);
  { std::lock_guard<std::mutex> lk(ctx->mu); }
  ctx->cv.notify_all();
}

}  // namespace

void VideoCtx::register_probe(GstPad* pad, gulong id) {
  if (!pad || id == 0) return;
  std::unique_lock<std::mutex> lk(probe_mu);
  if (probes_severed) {
    // Teardown already ran; a late live pad-added must not re-arm the race.
    // Remove outside the lock — gst_pad_remove_probe blocks on any in-flight
    // callback, and holding probe_mu across it is needless.
    lk.unlock();
    gst_pad_remove_probe(pad, id);
    return;
  }
  gst_object_ref(pad);
  probes.emplace_back(pad, id);
}

void VideoCtx::remove_probes() {
  std::vector<std::pair<GstPad*, gulong>> armed;
  {
    std::lock_guard<std::mutex> lk(probe_mu);
    probes_severed = true;
    armed.swap(probes);
  }
  // Outside the lock: gst_pad_remove_probe blocks until any running callback
  // returns, and a callback that ever took probe_mu would otherwise deadlock.
  for (const auto& [pad, id] : armed) {
    gst_pad_remove_probe(pad, id);
    gst_object_unref(pad);
  }
}

VideoCtx::~VideoCtx() {
  // Belt to the caller's braces: the pipeline should already be NULL and the
  // probes severed by the time we get here, but severing again is idempotent
  // and guards a VideoCtx freed on a path that skipped GstInserter teardown.
  remove_probes();
  if (h264_parser) gst_h264_nal_parser_free(h264_parser);
}

void record_sensor_timestamp(VideoCtx& video, std::span<const std::byte> pkt, std::int64_t pts_ns) {
  auto pkt_result = parse_packet(pkt);
  if (!pkt_result) return;
  const Packet& parsed = *pkt_result;
  std::span<const std::byte> raw;
  bool found = false;
  for (const auto& it : parsed.items) {
    if (it.tag == 2) {
      raw = it.value;
      found = true;
      break;
    }
  }
  if (!found) return;
  const Registry* reg = registry_by_key(parsed.ul_key);
  if (!reg) return;
  const ItemDescriptor* desc = reg->find(2);
  if (!desc) return;
  auto decoded = codec::decode(*desc, raw);
  if (!decoded) return;
  const std::uint64_t* p = std::get_if<std::uint64_t>(&*decoded);
  if (!p) return;
  const std::uint64_t sensor_timestamp_us = *p;
  const auto pts = static_cast<std::uint64_t>(pts_ns);
  std::lock_guard<std::mutex> lock(video.timestamp_mu);
  // Derive ST 0603 Time Status from absolute time against the media timeline.
  // No producer-side pruning: the map is consumed by the video-pad SEI probe
  // asynchronously, so KLV can be pushed more than a second ahead of the
  // frames being processed (bursty push, clock-paced replay, encoder
  // buffering). Eviction follows video-consumption progress in the SEI
  // injection path (upper_bound(pts) lookup), where entries with key <
  // frame_pts - kPtsMatchToleranceNs are discarded — under monotonic video
  // PTS those can never match a future frame. This bounds the map by the
  // actual KLV-vs-video lead (about one tolerance window at 30 fps) rather
  // than a wall-clock guess. If Generate is on and KLV is pushed far ahead
  // with video never arriving, entries accumulate until consumed — inherent,
  // as the data may still be needed.
  SensorTime entry{sensor_timestamp_us, kTimeStatusBase};
  if (video.have_prev_push) {
    entry.status = sensor_time_status(
        static_cast<std::int64_t>(sensor_timestamp_us) -
            static_cast<std::int64_t>(video.prev_push_ts_us),
        (static_cast<std::int64_t>(pts) - static_cast<std::int64_t>(video.prev_push_pts_ns)) /
            1000);
  }
  video.have_prev_push = true;
  video.prev_push_pts_ns = pts;
  video.prev_push_ts_us = sensor_timestamp_us;
  video.pts_to_sensor_timestamp[pts] = entry;
}

VideoSource parse_video_source(const std::string& raw) {
  if (raw.empty()) return {VideoSourceKind::None, {}};
  if (raw.rfind("pipeline:", 0) == 0) {
    std::string desc = raw.substr(9);
    if (desc.empty()) return {VideoSourceKind::Unsupported, raw};
    return {VideoSourceKind::Pipeline, desc};
  }
  if (raw.rfind("file:", 0) == 0) {
    return {VideoSourceKind::File, raw.substr(5)};
  }
  auto colon = raw.find(':');
  if (colon != std::string::npos) {
    std::string scheme = raw.substr(0, colon);
    bool valid = !scheme.empty() && std::isalpha(static_cast<unsigned char>(scheme[0]));
    for (size_t i = 1; i < scheme.size() && valid; ++i) {
      char c = scheme[i];
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.'))
        valid = false;
    }
    if (valid) {
      std::string low = scheme;
      std::transform(low.begin(), low.end(), low.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (low == "rtsp" || low == "rtsps") return {VideoSourceKind::Rtsp, raw};
      return {VideoSourceKind::Unsupported, raw};
    }
  }
  // Bare path
  std::error_code ec;
  bool exists = std::filesystem::exists(raw, ec);
  if (!ec && exists) return {VideoSourceKind::File, raw};
  return {VideoSourceKind::Unsupported, raw};
}

static Result<std::monostate> prepare_file_branch(GstElement* pipeline, GstPad* reserved_video_pad,
                                                  GstElement* mux, const std::string& video_path,
                                                  Sei0604 sei_0604,
                                                  std::unique_ptr<VideoCtx>& video) {
  const std::string container = sniff_container(video_path);
  const char* demuxer_name = demuxer_for_media_type(container);
  if (!demuxer_name) {
    g_warning("misbklv: video source '%s': %s is not a container this library demuxes",
              video_path.c_str(), container.empty() ? "an unrecognized format" : container.c_str());
    return Result<std::monostate>::err(Error::Unsupported);
  }
  GstElement* vsrc = gst_element_factory_make("filesrc", "vsrc");
  GstElement* parse = gst_element_factory_make(demuxer_name, "vparse");
  if (!vsrc || !parse) {
    if (vsrc) gst_object_unref(vsrc);
    if (parse) gst_object_unref(parse);
    g_warning("misbklv: could not create %s — is the plugin providing it installed?", demuxer_name);
    return Result<std::monostate>::err(Error::Backend);
  }
  g_object_set(vsrc, "location", video_path.c_str(), nullptr);
  video = std::make_unique<VideoCtx>();
  video->pipeline = pipeline;
  video->reserved_video_pad = reserved_video_pad;
  video->mux_element = mux;
  video->generate_sei = sei_0604 == Sei0604::Generate;
  if (video->generate_sei) video->h264_parser = gst_h264_nal_parser_new();
  g_signal_connect(parse, "pad-added", G_CALLBACK(on_video_pad_added), video.get());
  g_signal_connect(parse, "no-more-pads", G_CALLBACK(on_video_no_more_pads), video.get());
  gst_bin_add_many(GST_BIN(pipeline), vsrc, parse, nullptr);
  if (!gst_element_link(vsrc, parse)) return Result<std::monostate>::err(Error::Backend);
  // Dynamic pads arrive during PAUSED preroll. Wait before PLAYING so the
  // muxer's PMT includes both KLV and video even when callers push immediately.
  if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
    return Result<std::monostate>::err(Error::Backend);
  GstBus* bus = gst_element_get_bus(pipeline);
  const auto deadline = std::chrono::steady_clock::now() + kVideoPadTimeout;
  bool linked = false;
  int ignored = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(video->mu);
      if (video->cv.wait_for(lk, std::chrono::milliseconds(50),
                             [&] { return video->linked || video->no_more_pads; })) {
        linked = video->linked;
        ignored = video->ignored_video_pads;
        break;
      }
    }
    GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (msg) {
      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      // qtdemux can post not-linked transiently while its dynamic pad is being
      // connected. It is not an invalid source, so keep waiting for callback
      // state; other errors are surfaced here because finish cannot report a
      // message already consumed from this bus.
      const bool is_not_linked = (dbg && std::strstr(dbg, "not-linked"));
      if (!is_not_linked) {
        g_warning("misbklv: video source '%s': %s", video_path.c_str(),
                  err ? err->message : "pipeline error");
        if (err) g_error_free(err);
        g_free(dbg);
        gst_message_unref(msg);
        break;
      }
      if (err) g_error_free(err);
      g_free(dbg);
      gst_message_unref(msg);
    }
    if (std::chrono::steady_clock::now() > deadline) break;
  }
  gst_object_unref(bus);
  if (!linked) return Result<std::monostate>::err(Error::Unsupported);
  if (ignored)
    g_warning("misbklv: %d extra video stream(s) in '%s' not carried", ignored, video_path.c_str());
  return Result<std::monostate>::ok({});
}

bool object_within_branch(GstObject* src, GstElement* branch) {
  if (!src || !branch) return false;
  GstObject* cur = GST_OBJECT(gst_object_ref(src));
  while (cur) {
    if (cur == GST_OBJECT(branch)) {
      gst_object_unref(cur);
      return true;
    }
    GstObject* parent = gst_object_get_parent(cur);
    gst_object_unref(cur);
    cur = parent;
  }
  return false;
}

int rtsp_status_from_message(GstMessage* msg) {
  if (!msg) return 0;
  const GstStructure* details = nullptr;
  gst_message_parse_error_details(msg, &details);
  if (!details) return 0;
  // rtspsrc writes this field as G_TYPE_UINT, and the typed getters return
  // false on a type mismatch rather than converting — reading it as an int
  // silently yielded 0 for every real message, which fell back to the generic
  // resource-code rules and undid the status classification entirely. Accept
  // either spelling rather than depending on one.
  guint as_uint = 0;
  if (gst_structure_get_uint(details, "rtsp-status-code", &as_uint)) {
    return static_cast<int>(as_uint);
  }
  gint as_int = 0;
  if (gst_structure_get_int(details, "rtsp-status-code", &as_int)) return as_int;
  return 0;
}

Error classify_rtsp_error(GQuark domain, int code, int rtsp_status) {
  // A status at all means the server answered: it is reachable, and the
  // request was rejected on its merits. rtspsrc maps most non-2xx responses
  // onto RESOURCE/READ and 404 onto RESOURCE/NOT_FOUND, so without this the
  // codes below would read a bad path or an unsupported transport as an absent
  // source and retry it forever. Only a server-side 5xx — overloaded, briefly
  // broken — is worth coming back to.
  if (rtsp_status > 0) {
    return rtsp_status >= 500 ? Error::SourceUnavailable : Error::Unsupported;
  }
  // GST_RESOURCE_ERROR covers both directions of I/O, and only the read side
  // says the source is absent: refused, not found, not answering. The write
  // side belongs to an output — a full disk, a sink that cannot be opened —
  // which no amount of waiting for the aircraft will fix.
  if (domain == GST_RESOURCE_ERROR) {
    switch (code) {
      case GST_RESOURCE_ERROR_NOT_FOUND:
      case GST_RESOURCE_ERROR_OPEN_READ:
      case GST_RESOURCE_ERROR_OPEN_READ_WRITE:
      case GST_RESOURCE_ERROR_READ:
      case GST_RESOURCE_ERROR_BUSY:  // a camera with its client slot taken
        return Error::SourceUnavailable;
      default:
        // Authorization, and every write-side code: OPEN_WRITE, WRITE,
        // NO_SPACE_LEFT, CLOSE. Wrong credentials stay wrong, and a failing
        // output is not an absent source.
        return Error::Unsupported;
    }
  }
  // GST_STREAM_ERROR (codec not found, wrong type, decode) and GST_CORE_ERROR
  // (missing plugin, negotiation, pad) both mean the server answered and its
  // media cannot be handled by this build. Permanent.
  //
  // Anything else is deliberately permanent too: an unrecognized error must not
  // become an infinite retry in a consumer that polls on SourceUnavailable.
  return Error::Unsupported;
}

static Result<std::monostate> prepare_rtsp_branch(GstElement* pipeline, GstPad* reserved_video_pad,
                                                  GstElement* mux, const std::string& uri,
                                                  Sei0604 sei_0604,
                                                  std::unique_ptr<VideoCtx>& video) {
  GstElement* rtspsrc = gst_element_factory_make("rtspsrc", "rtspsrc");
  if (!rtspsrc) {
    g_warning("misbklv: rtspsrc element not available");
    return Result<std::monostate>::err(Error::Backend);
  }
  g_object_set(rtspsrc, "location", uri.c_str(), "latency", 0, "protocols", 0x07, nullptr);
  video = std::make_unique<VideoCtx>();
  video->pipeline = pipeline;
  video->reserved_video_pad = reserved_video_pad;
  video->mux_element = mux;
  video->is_live_unbounded = true;  // RTSP is always unbounded
  attach_delivery_probe(video.get());
  video->video_bin = rtspsrc;
  video->generate_sei = sei_0604 == Sei0604::Generate;
  if (video->generate_sei) video->h264_parser = gst_h264_nal_parser_new();
  g_signal_connect(rtspsrc, "pad-added", G_CALLBACK(on_rtspsrc_pad_added), video.get());
  // rtspsrc also emits no-more-pads, but for live we rely on async watch.
  gst_bin_add(GST_BIN(pipeline), rtspsrc);
  // For live, skip PAUSED wait. Set PAUSED then immediately PLAYING and watch 5s.
  if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
    return Result<std::monostate>::err(Error::Backend);
  }
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    return Result<std::monostate>::err(Error::Backend);
  }
  GstBus* bus = gst_element_get_bus(pipeline);
  const auto deadline = std::chrono::steady_clock::now() + kLivePadTimeout;
  bool linked = false;
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(video->mu);
      if (video->cv.wait_for(lk, std::chrono::milliseconds(50), [&] { return video->linked; })) {
        linked = video->linked;
        break;
      }
    }
    GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (msg) {
      GError* err = nullptr;
      gchar* dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      // This bus carries the whole pipeline. An error from the muxer or the
      // sink is an output fault, and says nothing about whether the source is
      // there — classifying it as absent would have a polling consumer retry a
      // full disk forever.
      const bool from_source = object_within_branch(GST_MESSAGE_SRC(msg), rtspsrc);
      const int rtsp_status = rtsp_status_from_message(msg);
      const Error klass =
          !from_source ? Error::Backend
          : err        ? classify_rtsp_error(err->domain, err->code, rtsp_status)
                       : Error::SourceUnavailable;
      g_warning("misbklv: RTSP pipeline error for '%s': %s%s", uri.c_str(),
                err ? err->message : "pipeline error",
                klass == Error::SourceUnavailable ? ""
                : from_source                     ? " (permanent)"
                                                  : " (from the output, not the source)");
      if (err) g_error_free(err);
      g_free(dbg);
      gst_message_unref(msg);
      gst_object_unref(bus);
      return Result<std::monostate>::err(klass);
    }
    if (std::chrono::steady_clock::now() > deadline) break;
  }
  gst_object_unref(bus);
  if (!linked) {
    // A pad appeared but nothing linked: the server answered and described
    // media this build cannot carry — an unsupported encoding, or a missing
    // depayloader/parser. Permanent, and retrying would spin. No pad at all
    // means nothing answered, which may be back later.
    const bool answered = video->saw_any_pad.load(std::memory_order_acquire);
    g_warning("misbklv: RTSP source '%s' produced no video pad within %lld s%s", uri.c_str(),
              static_cast<long long>(kLivePadTimeout.count()),
              answered ? " (source answered; its media is unusable here)" : "");
    return Result<std::monostate>::err(answered ? Error::Unsupported : Error::SourceUnavailable);
  }
  return Result<std::monostate>::ok({});
}

// Live branch: source never EOS; finish() sends EOS to the muxer after
// appsrc EOS and drains. File branch propagates demuxer EOS.
// prepare_pipeline_branch covers pipeline: live sources (is-live=true).
static Result<std::monostate> prepare_pipeline_branch(GstElement* pipeline,
                                                      GstPad* reserved_video_pad, GstElement* mux,
                                                      const std::string& desc, Sei0604 sei_0604,
                                                      std::unique_ptr<VideoCtx>& video) {
  // Narrowed grammar: pipeline: bin must expose a static src pad immediately.
  // Dynamic demuxers (tsdemux, qtdemux, matroskademux) expose pads only after
  // state changes and are not supported here.
  if (desc.find("demux") != std::string::npos) {
    g_warning("misbklv: pipeline: with demuxer not supported (needs static src pad)");
    return Result<std::monostate>::err(Error::Unsupported);
  }
  GError* err = nullptr;
  GstElement* bin = gst_parse_bin_from_description(desc.c_str(), TRUE, &err);
  if (!bin) {
    g_warning("misbklv: pipeline video_source failed to parse: %s",
              err ? err->message : "unknown error");
    if (err) g_error_free(err);
    return Result<std::monostate>::err(Error::Unsupported);
  }
  if (err) g_error_free(err);
  video = std::make_unique<VideoCtx>();
  video->pipeline = pipeline;
  video->reserved_video_pad = reserved_video_pad;
  video->mux_element = mux;
  video->is_live_unbounded = (desc.find("num-buffers") == std::string::npos);
  if (video->is_live_unbounded) attach_delivery_probe(video.get());
  video->video_bin = bin;
  video->generate_sei = sei_0604 == Sei0604::Generate;
  if (video->generate_sei) video->h264_parser = gst_h264_nal_parser_new();
  gst_bin_add(GST_BIN(pipeline), bin);
  // The bin's ghost src pad (created by parse_bin TRUE) is linked to reserved pad.
  // It may not be negotiated yet, but ghost pad exists immediately.
  GstPad* srcpad = gst_element_get_static_pad(bin, "src");
  if (!srcpad) {
    // Fallback: try to find any src ghost pad
    GstIterator* it = gst_element_iterate_src_pads(bin);
    gboolean done = FALSE;
    GValue val = G_VALUE_INIT;
    while (!done) {
      switch (gst_iterator_next(it, &val)) {
        case GST_ITERATOR_OK: {
          GstPad* p = static_cast<GstPad*>(g_value_get_object(&val));
          if (p && !srcpad) srcpad = static_cast<GstPad*>(gst_object_ref(p));
          g_value_reset(&val);
          break;
        }
        case GST_ITERATOR_DONE:
          done = TRUE;
          break;
        default:
          done = TRUE;
          break;
      }
    }
    g_value_unset(&val);
    gst_iterator_free(it);
  }
  bool linked = false;
  if (srcpad) {
    // Try direct pad link; no capsfilter needed as pipeline description should
    // already produce byte-stream parsers. Codec for Generate is verified
    // below: if caps already indicate non-H.264, the SEI probe is not attached
    // and video carries through unstamped; if caps are not yet negotiated the
    // probe is attached but per-buffer verifies H.264 to avoid corruption.
    GstPadLinkReturn ret = gst_pad_link(srcpad, reserved_video_pad);
    linked = (ret == GST_PAD_LINK_OK);
    if (!linked) {
      // Try through a queue + capsfilter if direct fails (caps negotiation)
      GstElement* queue = gst_element_factory_make("queue", nullptr);
      GstElement* cf = gst_element_factory_make("capsfilter", nullptr);
      if (queue) {
        gst_bin_add(GST_BIN(pipeline), queue);
        gst_element_sync_state_with_parent(queue);
        GstPad* qsink = gst_element_get_static_pad(queue, "sink");
        if (qsink && gst_pad_link(srcpad, qsink) == GST_PAD_LINK_OK) {
          GstPad* qsrc = gst_element_get_static_pad(queue, "src");
          if (qsrc) {
            if (cf) {
              GstCaps* fcaps = gst_caps_from_string("video/x-h264, stream-format=byte-stream");
              g_object_set(cf, "caps", fcaps, nullptr);
              gst_caps_unref(fcaps);
              gst_bin_add(GST_BIN(pipeline), cf);
              gst_element_sync_state_with_parent(cf);
              if (gst_element_link(queue, cf)) {
                GstPad* csrc = gst_element_get_static_pad(cf, "src");
                if (csrc) {
                  linked = gst_pad_link(csrc, reserved_video_pad) == GST_PAD_LINK_OK;
                  gst_object_unref(csrc);
                }
              }
            } else {
              linked = gst_pad_link(qsrc, reserved_video_pad) == GST_PAD_LINK_OK;
            }
            gst_object_unref(qsrc);
          }
          gst_object_unref(qsink);
        } else {
          if (qsink) gst_object_unref(qsink);
        }
      }
      if (!linked) g_warning("misbklv: failed to link pipeline bin to muxer: %d", ret);
    }
    if (linked && video->generate_sei && srcpad) {
      // Gate Generate probe on negotiated H.264 caps to avoid corrupting
      // non-H.264 bitstreams (e.g., x265enc ! h265parse). With the codec
      // latch, caps are resolved once via a CAPS event probe; the buffer
      // probe then fast-paths on the latched value, falling back to a
      // per-buffer check only while Unknown. Unknown (ghost pad before
      // PLAYING) still attaches both probes so the buffer path stays safe.
      // Prime the latch when caps are already negotiated to avoid staying
      // Unknown and retaining the per-frame fallback query this fix removes.
      GstCaps* caps = gst_pad_get_current_caps(srcpad);
      if (!caps) caps = gst_pad_query_caps(srcpad, nullptr);
      const CapsCodec cc = caps_codec(caps);
      if (caps) gst_caps_unref(caps);
      if (cc == CapsCodec::H264) {
        attach_generate_probes(srcpad, video.get());
        video->codec_latch.store(CodecLatch::IsH264, std::memory_order_relaxed);
      } else if (cc == CapsCodec::KnownNonH264) {
        g_warning("misbklv: Sei0604::Generate needs H.264 video; pipeline source carries non-H.264 "
                  "caps — carrying through unstamped");
      } else {
        // Caps not yet negotiated (common for ghost pad before PLAYING).
        // Attach both probes; buffer probe will fallback-check per frame until
        // the CAPS event latches.
        attach_generate_probes(srcpad, video.get());
      }
    }
    gst_object_unref(srcpad);
  } else {
    g_warning("misbklv: pipeline bin has no src ghost pad");
  }
  if (!linked) {
    // Pipeline bin linking failed — treat as Unsupported
    return Result<std::monostate>::err(Error::Unsupported);
  }
  // Mark as linked; no async wait needed since ghost pad is static.
  video->linked = true;
  // For live pipeline, also set PAUSED->PLAYING immediately without wait, similar to RTSP,
  // but we have already linked, so just advance states and return ok.
  // The caller (open_insert) will still set PLAYING, but we pre-set here to prime.
  // To avoid double state change, we set PAUSED then PLAYING now.
  if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
    return Result<std::monostate>::err(Error::Backend);
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    return Result<std::monostate>::err(Error::Backend);
  // Brief watch for bus ERROR to surface parse errors quickly, but not required for timeout.
  GstBus* bus = gst_element_get_bus(pipeline);
  GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
  if (msg) {
    GError* gerr = nullptr;
    gchar* dbg = nullptr;
    gst_message_parse_error(msg, &gerr, &dbg);
    g_warning("misbklv: pipeline video_source error: %s", gerr ? gerr->message : "unknown");
    if (gerr) g_error_free(gerr);
    g_free(dbg);
    gst_message_unref(msg);
    gst_object_unref(bus);
    return Result<std::monostate>::err(Error::Unsupported);
  }
  gst_object_unref(bus);
  return Result<std::monostate>::ok({});
}

Result<std::monostate> prepare_video_branch(GstElement* pipeline, GstPad* reserved_video_pad,
                                            GstElement* mux, const VideoSource& src,
                                            Sei0604 sei_0604, std::unique_ptr<VideoCtx>& video) {
  switch (src.kind) {
    case VideoSourceKind::File:
      return prepare_file_branch(pipeline, reserved_video_pad, mux, src.spec, sei_0604, video);
    case VideoSourceKind::Rtsp:
      return prepare_rtsp_branch(pipeline, reserved_video_pad, mux, src.spec, sei_0604, video);
    case VideoSourceKind::Pipeline:
      return prepare_pipeline_branch(pipeline, reserved_video_pad, mux, src.spec, sei_0604, video);
    case VideoSourceKind::None:
      return Result<std::monostate>::ok({});
    case VideoSourceKind::Unsupported:
    default:
      return Result<std::monostate>::err(Error::Unsupported);
  }
}

}  // namespace misbklv::detail
