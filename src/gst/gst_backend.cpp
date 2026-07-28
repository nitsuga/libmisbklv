// SPDX-License-Identifier: Apache-2.0
// gstreamer MediaBackend (ADR 0013). Extraction: {file|udp|srt}src ! tsdemux !
// appsink; reassemble appsink fragments and frame whole KLV packets (B0 spike).
// Insertion (B2): appsrc ! mpegtsmux ! {file|udp|srt}sink, optionally joined by
// a video passthrough branch filesrc ! demuxer (ADR 0020, ADR 0025). Live sources/sinks
// (udp/srt) add real-time pacing + idle-timeout termination (B4, ADR 0017).
#include "misbklv/gst_backend.hpp"
#include "misbklv/message.hpp"  // for KLV parsing in push()

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <gst/app/app.h>
#include <gst/base/gsttypefindhelper.h>
#include <gst/gst.h>
#define GST_USE_UNSTABLE_API
#include <gst/codecparsers/gsth264parser.h>
#undef GST_USE_UNSTABLE_API

#include "misbklv/packet.hpp"
#include "../pts_marks.hpp"

namespace misbklv {
namespace {

// Reassembly + framing state, shared with the appsink callbacks (one streaming
// thread; extract() only blocks on the bus while these run).
struct ExtractCtx {
  GstElement* sink = nullptr;
  const PacketHandler* on_packet = nullptr;
  std::vector<std::byte> reassembly;
  std::size_t packets = 0;  // whole KLV packets emitted (gates the idle timeout)
  detail::PtsMarks marks;   // buffer timestamps -> per-packet timestamps
  std::size_t stream_off = 0;  // absolute offset of reassembly[0] in the stream

  void drain() {
    std::size_t pos = 0;
    for (;;) {
      std::span<const std::byte> rest(reassembly.data() + pos, reassembly.size() - pos);
      const std::size_t n = packet_frame_length(rest);
      if (n == 0) break;  // need more data
      (*on_packet)(KlvPacket{rest.subspan(0, n), marks.at(stream_off + pos)});
      ++packets;
      pos += n;
    }
    if (pos) {
      reassembly.erase(reassembly.begin(), reassembly.begin() + pos);
      stream_off += pos;
    }
  }
};

void on_pad_added(GstElement*, GstPad* pad, gpointer user) {
  auto* ctx = static_cast<ExtractCtx*>(user);
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) caps = gst_pad_query_caps(pad, nullptr);
  const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  if (std::strcmp(name, "meta/x-klv") == 0) {
    GstPad* sinkpad = gst_element_get_static_pad(ctx->sink, "sink");
    if (!gst_pad_is_linked(sinkpad)) gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
  }
  gst_caps_unref(caps);
}

GstFlowReturn on_new_sample(GstElement* sink, gpointer user) {
  auto* ctx = static_cast<ExtractCtx*>(user);
  GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
  if (!sample) return GST_FLOW_OK;
  GstBuffer* buf = gst_sample_get_buffer(sample);
  GstMapInfo mi;
  if (buf && gst_buffer_map(buf, &mi, GST_MAP_READ)) {
    // Timestamp these bytes before they lose their identity in the reassembly
    // buffer (ADR 0021). The *running* time, not the raw buffer PTS: tsdemux's
    // segment is program-wide and starts at the earliest timestamp in the
    // program, so running time is nanoseconds from the start of the source —
    // the same timeline push() writes on, which is what makes read -> edit ->
    // write compose. An untimed buffer marks kNoPts.
    std::int64_t pts_ns = kNoPts;
    const GstSegment* seg = gst_sample_get_segment(sample);
    if (seg && GST_BUFFER_PTS_IS_VALID(buf)) {
      const GstClockTime rt =
          gst_segment_to_running_time(seg, GST_FORMAT_TIME, GST_BUFFER_PTS(buf));
      if (GST_CLOCK_TIME_IS_VALID(rt)) pts_ns = static_cast<std::int64_t>(rt);
    }
    ctx->marks.mark(ctx->stream_off + ctx->reassembly.size(), pts_ns);
    const auto* p = reinterpret_cast<const std::byte*>(mi.data);
    ctx->reassembly.insert(ctx->reassembly.end(), p, p + mi.size);
    gst_buffer_unmap(buf, &mi);
    ctx->drain();
  }
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

// A live source that has delivered data but then goes quiet this long is treated
// as ended (no EOS crosses udp/srt). Longer than the ~33 ms inter-packet gap, so
// it never trips mid-stream; short enough to end the extract promptly (B4).
inline constexpr guint64 kIdleTimeoutNs = 500'000'000;  // 500 ms

// How long finish() will wait for the pipeline to drain after EOS before giving
// up. This was an unbounded wait, which meant a video branch that never reaches
// EOS blocked the caller forever — it hung CI for six hours a run, and a
// consumer would simply never return from close().
//
// Generous on purpose: the drain has to carry whatever video is left, and a
// caller that pushed its KLV early can leave most of the file to remux here. So
// this is a *stall* guard, not a performance budget — it should only ever fire
// when nothing is happening at all.
inline constexpr GstClockTime kFinishDrainTimeout = 300 * GST_SECOND;  // 5 min

// Tolerance for PTS fuzzy matching when looking up sensorTimestamp for ST 0604
// SEI generation (fork 21). Video and KLV flow through separate gstreamer
// pipelines with different buffering, so video frames can arrive at the probe
// slightly ahead of their corresponding KLV packets in push(). At 30fps, 200ms
// = ~6 frames. Observed lag is typically submillisecond; 200ms provides ample
// headroom. Backward-only matching prevents incorrect matches across discontinuities.
inline constexpr uint64_t kPtsMatchToleranceNs = 200'000'000;  // 200 ms

// ST 0603.5 §7.4 Table 3 — the Time Status byte carried with a Precision Time
// Stamp. Bits 4-0 are reserved and always set.
inline constexpr uint8_t kTimeStatusReserved = 0b0001'1111;
inline constexpr uint8_t kTimeStatusLockUnknown = 0b1000'0000;    // bit 7
inline constexpr uint8_t kTimeStatusDiscontinuity = 0b0100'0000;  // bit 6
inline constexpr uint8_t kTimeStatusReverse = 0b0010'0000;        // bit 5
// Our floor: we relay a timestamp out of ST 0601 item 2, which says nothing
// about the source clock's lock state, so bit 7 is always Lock Unknown
// (ADR 0023). Bits 6/5 are derived per packet, below.
inline constexpr uint8_t kTimeStatusBase = kTimeStatusLockUnknown | kTimeStatusReserved;

// How far the KLV's absolute time may drift from the media timeline between two
// packets before it counts as a discontinuity rather than clock jitter. Both
// measure the same real seconds, so over a normal inter-packet gap they track
// each other to microseconds; a genuine break (a relock, a GPS fix, an edit) is
// tens of milliseconds at least. 50 ms sits well clear of both.
inline constexpr std::int64_t kTimeLinearToleranceUs = 50'000;

// The absolute time for one frame, and what ST 0603 says about it.
struct SensorTime {
  std::uint64_t timestamp_us = 0;
  std::uint8_t status = kTimeStatusBase;
};

// Build the source element from a spec: a bare path / "file:PATH" (EOS-ending),
// or a live "udp:HOST:PORT" / "srt:URI". Sets `*live` for the live schemes.
// Returns nullptr on a malformed spec.
GstElement* make_src(const std::string& spec, bool* live) {
  *live = false;
  const auto colon = spec.find(':');
  const std::string scheme =
      (colon == std::string::npos) ? std::string() : spec.substr(0, colon);
  if (scheme == "udp") {  // udp:HOST:PORT
    const std::string rest = spec.substr(colon + 1);
    const auto p = rest.rfind(':');
    if (p == std::string::npos) return nullptr;
    GstElement* s = gst_element_factory_make("udpsrc", "src");
    if (s) {
      GstCaps* caps = gst_caps_from_string(
          "video/mpegts, systemstream=(boolean)true, packetsize=(int)188");
      g_object_set(s, "address", rest.substr(0, p).c_str(), "port",
                   std::atoi(rest.substr(p + 1).c_str()), "caps", caps, "timeout",
                   kIdleTimeoutNs, nullptr);
      gst_caps_unref(caps);
      *live = true;
    }
    return s;
  }
  if (scheme == "srt") {
    GstElement* s = gst_element_factory_make("srtsrc", "src");
    if (s) g_object_set(s, "uri", spec.c_str(), nullptr);  // srt://...
    if (s) *live = true;
    return s;
  }
  const std::string path = (scheme == "file") ? spec.substr(colon + 1) : spec;
  GstElement* s = gst_element_factory_make("filesrc", "src");
  if (s) g_object_set(s, "location", path.c_str(), nullptr);
  return s;
}

// Build the sink element from an InsertConfig spec: "file:PATH" | "udp:HOST:PORT"
// | "srt:URI". Returns nullptr on an unknown scheme.
GstElement* make_sink(const std::string& spec) {
  const auto colon = spec.find(':');
  if (colon == std::string::npos) return nullptr;
  const std::string scheme = spec.substr(0, colon);
  const std::string rest = spec.substr(colon + 1);
  if (scheme == "file") {
    GstElement* s = gst_element_factory_make("filesink", "sink");
    if (s) g_object_set(s, "location", rest.c_str(), nullptr);
    return s;
  }
  if (scheme == "udp") {  // udp:HOST:PORT
    const auto p = rest.rfind(':');
    if (p == std::string::npos) return nullptr;
    GstElement* s = gst_element_factory_make("udpsink", "sink");
    if (s) g_object_set(s, "host", rest.substr(0, p).c_str(), "port",
                        std::atoi(rest.substr(p + 1).c_str()), nullptr);
    return s;
  }
  if (scheme == "srt") {
    GstElement* s = gst_element_factory_make("srtsink", "sink");
    if (s) g_object_set(s, "uri", spec.c_str(), nullptr);  // srt://...
    return s;
  }
  return nullptr;
}

// --- video passthrough branch (ADR 0020) ------------------------------------
// State shared with the demuxer's dynamic-pad callbacks. A demuxer exposes one pad
// per elementary stream once it has parsed the container; we link the FIRST
// video/* pad to a mpegtsmux request pad and ignore every other pad (audio,
// subtitles, and any KLV the source already carries — the caller supplies its
// own). open_insert() waits on `cv` for that pad, so the muxer never writes a
// PMT before the video stream has joined. Lives as long as the pipeline.
//
// Bound on that wait: parsing a container header is fast (milliseconds), so this
// only ever fires on a source that never yields a pad at all.
inline constexpr std::chrono::seconds kVideoPadTimeout{10};

struct VideoCtx {
  GstElement* mux = nullptr;
  GstElement* pipeline = nullptr;  // for creating fakesinks
  std::mutex mu;
  std::condition_variable cv;
  bool linked = false;         // a video pad reached the muxer
  bool no_more_pads = false;   // the demuxer is done exposing pads
  int ignored_video_pads = 0;  // 2nd+ video stream: carried? no. recorded? yes.
  bool sei_codec_unsupported = false;  // Generate asked for, video isn't H.264
  bool generate_sei = false;   // generate ST 0604 SEI from frame PTS (fork 21)

  // PTS (ns) → sensorTimestamp (µs) mapping for ST 0604 SEI generation (fork 21).
  // Populated from KLV packets in push(), looked up in video pad probe.
  std::mutex timestamp_mu;
  std::map<uint64_t, SensorTime> pts_to_sensor_timestamp;
  // Last packet pushed, for deriving the Time Status of the next one. Written
  // under timestamp_mu from push() only.
  bool have_prev_push = false;
  std::uint64_t prev_push_pts_ns = 0;
  std::uint64_t prev_push_ts_us = 0;

  // H.264 NAL/SEI parser for the SEI probe (fork 21). Owned for the session and
  // touched only from the streaming thread inside the probe, so it needs no lock.
  GstH264NalParser* h264_parser = nullptr;

  ~VideoCtx() {
    if (h264_parser) gst_h264_nal_parser_free(h264_parser);
  }
};

// True for caps whose media type is video/* (video/x-h264, video/x-h265, ...).
bool caps_are_video(GstCaps* caps) {
  if (!caps || gst_caps_is_empty(caps)) return false;
  const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  return name && std::strncmp(name, "video/", 6) == 0;
}

// The caps' media type, or "" — "video/x-h264", "video/x-h265", "video/mpeg", ...
std::string caps_media_type(GstCaps* caps) {
  if (!caps || gst_caps_is_empty(caps)) return {};
  const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  return name ? std::string(name) : std::string();
}

// The demuxer for a container, or nullptr for "we do not carry video out of
// this".
//
// This used to be parsebin's job (ADR 0020), and parsebin did it well until we
// tried to ship a bundle with only the plugins this library uses. parsebin
// decides a stream is fully parsed by asking whether any *decoder* in the
// registry accepts its caps — a decoder it never instantiates. On a normal
// system one always exists, so the dependency is invisible; on a minimal one
// there is none, parsebin never reaches "final caps", and it fails with
// "no suitable plugins found" for a stream it has already parsed correctly.
//
// Requiring an H.264 decoder to be installed in order to *not* decode H.264 is
// not a dependency this library can carry, so the container table is explicit
// now — the same shape as the parser table below, and honest about what it
// supports. See ADR 0025.
const char* demuxer_for_media_type(const std::string& type) {
  if (type == "video/quicktime") return "qtdemux";      // MP4 / MOV
  if (type == "video/mpegts") return "tsdemux";
  if (type == "video/x-matroska") return "matroskademux";
  return nullptr;
}

// Sniff a file's container from its opening bytes.
//
// Doing this up front beats a have-type callback: the path is known before the
// pipeline exists, so the container is settled and `filesrc ! demuxer` is built
// in one piece, with no dynamic element to sequence against preroll.
//
// 16 KiB is generous — every container we accept identifies itself in the first
// few hundred bytes — and a low-confidence guess is treated as no answer, so an
// unsupported file is refused rather than half-plugged.
std::string sniff_container(const std::string& path) {
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

// The parser this codec needs between the demuxer and mpegtsmux, or nullptr for
// "link it straight through".
//
// A demuxer hands H.26x out of an MP4 in its container form (avc/hvc1, with
// codec_data), and mpegtsmux wants byte-stream — so those two need a parser to
// convert.
//
// MPEG-1/2 is here for a subtler reason, and it is the one thing parsebin did
// for us that a bare demuxer does not: parsebin plugged a parser for *every*
// stream, including codecs absent from this table, so what reached the muxer was
// always parsed. A demuxer alone hands MPEG-2 out of a TS unparsed, mpegtsmux
// will not take it, and the failure surfaces at finish() rather than at link
// time. So the table now has to name every codec the muxer needs framed, not
// just the ones needing a format conversion (ADR 0025).
//
// Parsers are idempotent — one in front of an already-parsed stream is a
// passthrough — so listing a codec here costs nothing when the demuxer happened
// to parse it already.
const char* parser_for_media_type(const std::string& type) {
  if (type == "video/x-h264") return "h264parse";
  if (type == "video/x-h265") return "h265parse";
  if (type == "video/mpeg") return "mpegvideoparse";     // MPEG-1/2 video
  if (type == "video/x-h263") return "h263parse";
  if (type == "video/x-vp9") return "vp9parse";
  return nullptr;
}

// Generate ST 0604 Precision Time Stamp SEI payload (fork 21, ADR 0023).
// Returns SEI payload: type (5) + length (28) + UUID (16) + status (1) + timestamp (11).
// Layout per ST 0604.6 §7 (UUID, status byte, 0xFF emulation prevention Table 2).
std::vector<std::byte> generate_0604_sei_payload(uint64_t timestamp_microsec,
                                                 uint8_t time_status) {
  std::vector<std::byte> payload;

  // SEI User Unregistered: type 5, payload length 28
  payload.push_back(std::byte{5});   // payload type
  payload.push_back(std::byte{28});  // payload size (16-byte UUID + 1 status + 11 timestamp)

  // UUID "MISPmicrosectime" (16 bytes) per ST 0604.6 §7.2
  const uint8_t uuid[16] = {'M','I','S','P','m','i','c','r','o','s','e','c','t','i','m','e'};
  for (int i = 0; i < 16; i++) {
    payload.push_back(std::byte{uuid[i]});
  }

  // Time Status (1 byte) per ST 0603.5 §7.4 Table 3 — derived in push(), see
  // sensor_time_status().
  payload.push_back(std::byte{time_status});

  // Modified Precision Time Stamp (11 bytes): the 8-byte big-endian timestamp in
  // 2-byte groups separated by 0xFF emulation-prevention bytes, per ST 0604.6
  // §7.4 Table 2 (bytes 18-28, byte 18 most significant). Extracted by shifting
  // rather than aliasing the uint64 so the output does not depend on host endianness.
  const auto ts_byte = [timestamp_microsec](unsigned i) {  // i=0 is most significant
    return std::byte{static_cast<uint8_t>((timestamp_microsec >> (8 * (7 - i))) & 0xFF)};
  };
  payload.push_back(ts_byte(0));       // Bytes 18,19
  payload.push_back(ts_byte(1));
  payload.push_back(std::byte{0xFF});  // Byte 20
  payload.push_back(ts_byte(2));       // Bytes 21,22
  payload.push_back(ts_byte(3));
  payload.push_back(std::byte{0xFF});  // Byte 23
  payload.push_back(ts_byte(4));       // Bytes 24,25
  payload.push_back(ts_byte(5));
  payload.push_back(std::byte{0xFF});  // Byte 26
  payload.push_back(ts_byte(6));       // Bytes 27,28
  payload.push_back(ts_byte(7));

  return payload;
}

// Assemble the complete ST 0604 SEI NAL unit: 3-byte start code, SEI NAL header,
// the §7 payload, and the RBSP stop bit.
//
// The payload is assembled by hand rather than via gst_h264_create_sei_memory()
// because gstreamer 1.20's SEI writer has no user_data_unregistered payload type
// (GstH264SEIPayloadType gained it later). Everything else — NAL boundaries, SEI
// message parsing — goes through the codecparsers API.
std::vector<std::byte> build_0604_sei_nal(const SensorTime& t) {
  std::vector<std::byte> nal{std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
                             std::byte{0x06}};  // start code + NAL type 6 (SEI)
  const auto payload = generate_0604_sei_payload(t.timestamp_us, t.status);
  nal.insert(nal.end(), payload.begin(), payload.end());
  nal.push_back(std::byte{0x80});  // RBSP stop bit
  return nal;
}

// Time Status for a KLV packet, from how its absolute time moved relative to the
// media timeline since the previous packet (ST 0603.5 §7.4 Table 3).
//
// Both clocks measure the same real seconds, so in normal running the absolute
// timestamp advances by the same amount as the presentation timestamp. When it
// does not, time did not increment forward linearly — which is exactly what
// bit 6 reports; bit 5 then says which way it jumped. The first packet has
// nothing to compare against and is reported Normal.
std::uint8_t sensor_time_status(std::int64_t delta_ts_us, std::int64_t delta_pts_us) {
  if (delta_ts_us < 0)  // absolute time went backwards
    return kTimeStatusBase | kTimeStatusDiscontinuity | kTimeStatusReverse;
  if (std::llabs(delta_ts_us - delta_pts_us) > kTimeLinearToleranceUs)
    return kTimeStatusBase | kTimeStatusDiscontinuity;  // forward, but not linear
  return kTimeStatusBase;
}

// True for VCL NAL types — the coded slices an SEI must precede within its
// access unit.
bool nal_is_vcl(guint type) {
  return (type >= GST_H264_NAL_SLICE && type <= GST_H264_NAL_SLICE_IDR) ||
         type == GST_H264_NAL_SLICE_AUX || type == GST_H264_NAL_SLICE_EXT ||
         type == GST_H264_NAL_SLICE_DEPTH;
}

// True if `msg` is one Generate mode replaces: Picture Timing (whose absence is
// what quiets the SPS-association warning), or an ST 0604 Precision Time Stamp
// the source already carried — we are about to write our own, and two of them in
// one access unit leaves a reader guessing which is authoritative (ADR 0024).
//
// How a source ST 0604 SEI arrives depends on the gstreamer version, and getting
// this wrong is silent: the message is simply not recognised, so the source's
// timestamp is left in place next to ours. 1.22 added a parsed payload type for
// user_data_unregistered; before that it came through as an unhandled payload
// and had to be matched by its raw type 5 plus the §7.1 identifier.
bool sei_message_is_replaced(const GstH264SEIMessage& msg) {
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

// True if every SEI message in this NAL is one we replace — only then is
// dropping the whole NAL lossless. A NAL mixing a replaced message with an
// unrelated one (buffering period, recovery point) is left alone rather than
// taking the bystander with it.
bool sei_nal_is_replaced(GstH264NalParser* parser, GstH264NalUnit* nalu) {
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

// Pad probe: generate an ST 0604 SEI for each H.264 access unit and inject it
// before the first slice, stripping any Picture Timing SEI the source carried
// (fork 21, ADR 0023).
//
// NAL walking and SEI parsing go through gstreamer's codecparsers rather than a
// hand-rolled byte scan: it gets start-code length, NAL boundaries and the SEI
// 0xFF-continuation syntax right. All positions are tracked as *offsets* into a
// single mapping — pointers taken from one gst_buffer_map() are not valid across
// an unmap/remap, since mapping a multi-memory buffer can return a fresh merged
// allocation each time.
GstPadProbeReturn on_h264_buffer_inject_sei(GstPad*, GstPadProbeInfo* info,
                                              gpointer user) {
  auto* ctx = static_cast<VideoCtx*>(user);
  if (!ctx->generate_sei || !ctx->h264_parser) return GST_PAD_PROBE_OK;

  GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buffer || !GST_BUFFER_PTS_IS_VALID(buffer)) return GST_PAD_PROBE_OK;

  // Absolute sensorTimestamp (µs) for this frame, matched from KLV by PTS.
  // Backward-only: a frame is never given a timestamp from a later KLV packet.
  // If nothing matches within tolerance we write no SEI — a relative-PTS
  // fallback would produce a well-formed ST 0604 timestamp near 1970 that a
  // reader cannot distinguish from a real one.
  //
  // An unmatched frame is still *scanned*: under Generate the KLV is the single
  // timestamp authority for the stream, so a source ST 0604 is removed whether or
  // not we have something to put in its place. Leaving it would make provenance
  // vary frame to frame with nothing in the stream to signal which is which
  // (ADR 0024).
  const uint64_t pts_ns = GST_BUFFER_PTS(buffer);
  SensorTime sensor_time;
  bool matched = false;
  {
    std::lock_guard<std::mutex> lock(ctx->timestamp_mu);
    auto it = ctx->pts_to_sensor_timestamp.upper_bound(pts_ns);
    if (it != ctx->pts_to_sensor_timestamp.begin()) {
      --it;  // first entry at or before pts_ns
      if (pts_ns - it->first <= kPtsMatchToleranceNs) {
        sensor_time = it->second;
        matched = true;
      }
    }
  }
  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;

  // Single pass: collect Picture Timing SEI NALs to drop (as [start,end) offsets,
  // ascending and non-overlapping) and the offset of the first VCL NAL.
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

    if (res == GST_H264_PARSER_NO_NAL_END) break;  // this NAL ran to end of data
    offset = nalu.offset + nalu.size;
  }

  // Inject only with both a timestamp and a slice to put it before. With
  // neither and nothing to strip, the access unit is already what we want.
  const bool inject = matched && have_insert;
  if (!inject && strip.empty()) {
    gst_buffer_unmap(buffer, &map);
    return GST_PAD_PROBE_OK;
  }

  const auto sei_nal = inject ? build_0604_sei_nal(sensor_time) : std::vector<std::byte>{};
  gsize stripped = 0;
  for (const auto& [s, e] : strip) stripped += e - s;
  const gsize new_size = map.size - stripped + sei_nal.size();
  if (new_size == 0) {  // an access unit that was nothing but replaced SEI
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
    gst_buffer_unref(new_buffer);  // else the allocation leaks
    gst_buffer_unmap(buffer, &map);
    return GST_PAD_PROBE_OK;
  }

  // Copy the access unit in runs, skipping stripped ranges and splicing the new
  // SEI in at the first slice.
  guint8* dst = new_map.data;
  gsize pos = 0;
  std::size_t si = 0;
  bool inserted = !inject;  // nothing to insert counts as already done
  while (pos < map.size) {
    if (!inserted && pos == insert_at) {
      std::memcpy(dst, sei_nal.data(), sei_nal.size());
      dst += sei_nal.size();
      inserted = true;
    }
    if (si < strip.size() && pos == strip[si].first) {
      pos = strip[si].second;  // drop this Picture Timing SEI
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

  // Defensive: if the copy did not fill the buffer exactly, our size arithmetic
  // and our copy disagree. Pass the original through untouched rather than emit
  // a buffer with an uninitialised tail.
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

// Link a pad we are not carrying to a fakesink. The demuxer requires every pad it
// exposes to be linked; an unlinked one errors the whole pipeline as not-linked.
void drop_pad_to_fakesink(GstPad* pad, VideoCtx* ctx) {
  // queue ! fakesink, and the queue is the part that matters.
  //
  // A demuxer pushes every stream it has from one streaming thread. A sink in
  // PAUSED takes a buffer, prerolls, and then blocks that thread until the
  // pipeline reaches PLAYING — so dropping the source's audio or metadata
  // straight into a sink stops the video queued behind it on the same thread.
  // The muxer then never prerolls, so the pipeline never reaches PLAYING, so
  // the sink never unblocks: a circular preroll deadlock. (Not hypothetical —
  // it hung CI on every run under gstreamer 1.24.)
  //
  // The queue gives this branch its own thread, so the demuxer's thread returns
  // immediately and keeps feeding the video. `leaky=downstream` means a stream
  // we are discarding can never apply backpressure to one we are carrying.
  GstElement* queue = gst_element_factory_make("queue", nullptr);
  GstElement* fakesink = gst_element_factory_make("fakesink", nullptr);
  if (!queue || !fakesink) {
    if (queue) gst_object_unref(queue);
    if (fakesink) gst_object_unref(fakesink);
    return;
  }
  g_object_set(queue, "leaky", 2 /* downstream */, "max-size-buffers", 5,
               "max-size-bytes", 0, "max-size-time", G_GUINT64_CONSTANT(0), nullptr);
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

  // ST 0604 SEI generation is H.264-only (ADR 0024). Asking for it on another
  // codec is a caller error, not something to do quietly by half: refuse the
  // session rather than write an output whose video silently has no timestamps.
  if (video && ctx->generate_sei && media_type != "video/x-h264") {
    std::lock_guard<std::mutex> lk(ctx->mu);
    if (!ctx->linked) {
      g_warning("misbklv: Sei0604::Generate needs H.264 video; source carries %s",
                media_type.empty() ? "an unknown codec" : media_type.c_str());
      ctx->sei_codec_unsupported = true;
      drop_pad_to_fakesink(pad, ctx);
      ctx->cv.notify_all();
      return;
    }
  }

  if (!video) {  // audio / subtitles / source-side KLV: carried by nothing
    drop_pad_to_fakesink(pad, ctx);
    return;
  }

  std::lock_guard<std::mutex> lk(ctx->mu);
  if (ctx->linked) {  // a second video stream: carry the first, note this one
    ++ctx->ignored_video_pads;
    // ...and still link it, to a fakesink. The demuxer requires every pad it
    // exposes to be linked; leaving this one dangling posts a "not-linked"
    // error that the wait loop below treats as transient and swallows, so the
    // pipeline stalls with nothing reported. Non-video pads have been dropped
    // this way since the MP4 audio fix; this path was missed.
    drop_pad_to_fakesink(pad, ctx);
    return;
  }

  // H.26x arrives from an MP4 in container form and the muxer wants byte-stream,
  // so those need a parser in between; other codecs link straight through.
  const char* parser_name = parser_for_media_type(media_type);
  if (!parser_name) {
    GstPad* mux_sink = gst_element_request_pad_simple(ctx->mux, "sink_%d");
    if (mux_sink && gst_pad_link(pad, mux_sink) == GST_PAD_LINK_OK)
      ctx->linked = true;
    else
      g_warning("misbklv: failed to link %s pad to muxer", media_type.c_str());
    if (mux_sink) gst_object_unref(mux_sink);
    ctx->cv.notify_all();
    return;
  }

  GstElement* parse = gst_element_factory_make(parser_name, nullptr);
  if (!parse) {
    g_warning("misbklv: failed to create %s element", parser_name);
    ctx->cv.notify_all();
    return;
  }

  // Under Generate we are rewriting the access unit anyway, so repeat SPS/PPS on
  // every IDR: it keeps parameter sets available for a reader that joins mid-
  // stream and parses our SEI. Under Preserve we set nothing — passthrough means
  // the elementary stream comes out as it went in (ADR 0024).
  if (ctx->generate_sei) g_object_set(parse, "config-interval", -1, nullptr);

  gst_bin_add(GST_BIN(ctx->pipeline), parse);
  gst_element_sync_state_with_parent(parse);

  // Link: demuxer pad -> h264parse -> muxer
  GstPad* parse_sink = gst_element_get_static_pad(parse, "sink");
  if (!parse_sink) {
    g_warning("misbklv: failed to get %s sink pad", parser_name);
    ctx->cv.notify_all();
    return;
  }

  GstPadLinkReturn ret = gst_pad_link(pad, parse_sink);
  gst_object_unref(parse_sink);

  if (ret != GST_PAD_LINK_OK) {
    g_warning("misbklv: failed to link video pad to %s: %d (demuxer parent: %s, parser parent: %s)",
              parser_name, ret,
              GST_ELEMENT_NAME(gst_pad_get_parent_element(pad)),
              GST_ELEMENT_NAME(GST_ELEMENT_PARENT(parse)));
    ctx->cv.notify_all();
    return;
  }

  // Link parser to muxer using gst_element_link (simpler, handles pad requests)
  if (gst_element_link(parse, ctx->mux)) {
    ctx->linked = true;

    // ST 0604 SEI generation (ADR 0023/0024) — H.264 only, guaranteed above.
    if (ctx->generate_sei) {
      GstPad* parse_src = gst_element_get_static_pad(parse, "src");
      if (parse_src) {
        gst_pad_add_probe(parse_src, GST_PAD_PROBE_TYPE_BUFFER,
                          on_h264_buffer_inject_sei, ctx, nullptr);
        gst_object_unref(parse_src);
      }
    }
  } else {
    g_warning("misbklv: failed to link %s to muxer", parser_name);
  }
  ctx->cv.notify_all();  // wake open_insert (linked, or a failure it will time out on)
}

void on_video_no_more_pads(GstElement*, gpointer user) {
  auto* ctx = static_cast<VideoCtx*>(user);
  {
    std::lock_guard<std::mutex> lk(ctx->mu);
    ctx->no_more_pads = true;
  }
  ctx->cv.notify_all();
}

// appsrc(meta/x-klv) ! mpegtsmux ! sink. push() blocks on appsrc backpressure;
// finish() sends EOS and drains. Stock mpegtsmux (gst >= 1.20) already emits
// stream_type 0x06 + KLVA, so no PMT rewrite is needed (fork 13; verified by
// round-trip).
class GstInserter : public Inserter {
 public:
  // `removable_sink` is the sink's file path IF this session created it and may
  // therefore delete it again — empty for a non-file sink, or for a path that
  // already existed (that file is the caller's, ADR 0022).
  GstInserter(GstElement* pipeline, GstElement* appsrc,
              std::unique_ptr<VideoCtx> video = nullptr,
              std::string removable_sink = {})
      : pipeline_(pipeline), appsrc_(appsrc), video_(std::move(video)),
        removable_sink_(std::move(removable_sink)) {}
  ~GstInserter() override {
    if (pipeline_) {
      gst_element_set_state(pipeline_, GST_STATE_NULL);  // joins the streaming
      gst_object_unref(pipeline_);  // threads, so video_ outlives its callbacks
    }
    // An abandoned session — destroyed without a finish() that returned ok — has
    // not produced output, only an unfinalized file. Same guarantee as a failed
    // finish(): no error path leaves a file behind (ADR 0022).
    discard_output();
  }

  Result<std::monostate> push(std::span<const std::byte> pkt,
                              std::int64_t pts_ns) override {
    // With a video branch both streams must share the source's timeline, so the
    // synthesized ~30 fps counter is a caller error, not a fallback (ADR 0020).
    if (video_ && pts_ns == kNoPts)
      return Result<std::monostate>::err(Error::Unsupported);

    // If video passthrough + SEI generation: parse KLV to extract sensorTimestamp
    // (tag 2) and map it to this packet's PTS for ST 0604 SEI generation (fork 21).
    if (video_ && video_->generate_sei && pts_ns != kNoPts) {
      auto msg_result = Message::parse(pkt);
      if (msg_result) {
        auto& msg = *msg_result;
        // ST 0601 sensorTimestamp is tag 2, type uint64 (microseconds)
        if (msg.has(2)) {
          auto ts_result = msg.get<std::uint64_t>(2);
          if (ts_result) {
            const std::uint64_t sensor_timestamp_us = *ts_result;
            const auto pts = static_cast<std::uint64_t>(pts_ns);
            std::lock_guard<std::mutex> lock(video_->timestamp_mu);

            // Derive the ST 0603 Time Status from how absolute time moved
            // against the media timeline since the last packet (ADR 0024).
            SensorTime entry{sensor_timestamp_us, kTimeStatusBase};
            if (video_->have_prev_push) {
              entry.status = sensor_time_status(
                  static_cast<std::int64_t>(sensor_timestamp_us) -
                      static_cast<std::int64_t>(video_->prev_push_ts_us),
                  (static_cast<std::int64_t>(pts) -
                   static_cast<std::int64_t>(video_->prev_push_pts_ns)) / 1000);
            }
            video_->have_prev_push = true;
            video_->prev_push_pts_ns = pts;
            video_->prev_push_ts_us = sensor_timestamp_us;

            video_->pts_to_sensor_timestamp[pts] = entry;
            // No pruning: the map persists for the session. Video can lag KLV
            // push() by many seconds, so old entries still get looked up.
          }
        }
      }
    }

    GstBuffer* buf = gst_buffer_new_allocate(nullptr, pkt.size(), nullptr);
    gst_buffer_fill(buf, 0, pkt.data(), pkt.size());
    GST_BUFFER_PTS(buf) = (pts_ns == kNoPts) ? pts_
                                             : static_cast<GstClockTime>(pts_ns);
    GST_BUFFER_DURATION(buf) = kFrameDur;
    pts_ += kFrameDur;
    const GstFlowReturn ret =
        gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf);  // consumes buf
    return ret == GST_FLOW_OK ? Result<std::monostate>::ok({})
                              : Result<std::monostate>::err(Error::Backend);
  }

  Result<std::monostate> finish() override {
    gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
    GstBus* bus = gst_element_get_bus(pipeline_);
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, kFinishDrainTimeout,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    // Only a clean EOS is success. A null message is the timeout above — the
    // pipeline stalled — and is a failure like any other, so the ADR 0022
    // cleanup below runs rather than leaving a half-written output behind.
    const bool ok = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (!msg)
      g_warning("misbklv: pipeline did not drain within %" G_GUINT64_FORMAT
                "s of EOS; giving up",
                static_cast<guint64>(kFinishDrainTimeout / GST_SECOND));
    if (msg) gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline_, GST_STATE_NULL);  // flush/close the sink
    if (ok)
      removable_sink_.clear();  // this is the output now; never delete it
    else
      discard_output();  // an error result leaves no output file (ADR 0022)
    return ok ? Result<std::monostate>::ok({})
              : Result<std::monostate>::err(Error::Backend);
  }

 private:
  // Remove the sink file this session created, if it still owns one. Called
  // after the pipeline is in NULL, so the sink has closed the file first.
  void discard_output() {
    if (removable_sink_.empty()) return;
    std::remove(removable_sink_.c_str());
    removable_sink_.clear();
  }

  static constexpr GstClockTime kFrameDur = 33'000'000;  // ~30 fps pacing
  GstElement* pipeline_;
  GstElement* appsrc_;
  std::unique_ptr<VideoCtx> video_;  // null = KLV-only pipeline
  GstClockTime pts_ = 0;
  std::string removable_sink_;  // empty once there is nothing we may delete
};

class GstBackend : public MediaBackend {
 public:
  Result<std::monostate> extract(std::string_view source,
                                 const PacketHandler& on_packet,
                                 std::stop_token stop = {}) override {
    gst_init(nullptr, nullptr);  // idempotent
    GstElement* pipeline = gst_pipeline_new("misbklv-extract");
    bool live = false;
    GstElement* src = make_src(std::string(source), &live);
    GstElement* demux = gst_element_factory_make("tsdemux", "demux");
    GstElement* sink = gst_element_factory_make("appsink", "sink");
    if (!pipeline || !src || !demux || !sink) {
      if (src) gst_object_unref(src);
      if (demux) gst_object_unref(demux);
      if (sink) gst_object_unref(sink);
      if (pipeline) gst_object_unref(pipeline);
      return Result<std::monostate>::err(Error::Backend);
    }
    g_object_set(sink, "emit-signals", TRUE, "sync", FALSE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), src, demux, sink, nullptr);
    gst_element_link(src, demux);

    ExtractCtx ctx;
    ctx.sink = sink;
    ctx.on_packet = &on_packet;
    g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), &ctx);
    g_signal_connect(sink, "new-sample", G_CALLBACK(on_new_sample), &ctx);

    bool ok = true;
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      ok = false;
    } else {
      GstBus* bus = gst_element_get_bus(pipeline);
      // A file source ends at EOS; a live source never does, so udpsrc posts a
      // GstUDPSrcTimeout ELEMENT message once it idles — we treat that as end,
      // but only after data has flowed (an early timeout during startup, before
      // the sender is up, is ignored so it can't truncate the stream). A finite
      // pop timeout lets us also poll `stop` for cooperative early cancel (ADR
      // 0019) — the exit path for a KlvStream consumer that breaks.
      constexpr GstClockTime kPoll = 100 * GST_MSECOND;
      for (;;) {
        if (stop.stop_requested()) break;  // cancelled (ok stays true)
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus, kPoll,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR |
                                        GST_MESSAGE_ELEMENT));
        if (!msg) continue;  // timeout -> re-check stop / keep waiting
        const GstMessageType t = GST_MESSAGE_TYPE(msg);
        bool done = (t == GST_MESSAGE_EOS);
        if (t == GST_MESSAGE_ERROR) {
          ok = false;
          done = true;
        } else if (t == GST_MESSAGE_ELEMENT && live) {
          const GstStructure* s = gst_message_get_structure(msg);
          if (s && gst_structure_has_name(s, "GstUDPSrcTimeout") && ctx.packets > 0)
            done = true;
        }
        gst_message_unref(msg);
        if (done) break;
      }
      gst_object_unref(bus);
    }
    // set_state(NULL) blocks until streaming threads stop, so ctx (and the
    // borrowed callback) outlive all callbacks.
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return ok ? Result<std::monostate>::ok({})
              : Result<std::monostate>::err(Error::Backend);
  }

  Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig& cfg) override {
    using R = Result<std::unique_ptr<Inserter>>;
    gst_init(nullptr, nullptr);
    // Validate the video source BEFORE anything is built: a filesink creates its
    // file the moment the pipeline leaves NULL, and a failed open_insert must not
    // leave a partial output behind (ADR 0020).
    std::string video_path;
    if (!cfg.video_source.empty()) {
      if (cfg.realtime) return R::err(Error::Unsupported);  // untested pairing
      video_path = cfg.video_source;
      if (video_path.rfind("file:", 0) == 0) video_path.erase(0, 5);
      std::FILE* f = std::fopen(video_path.c_str(), "rb");
      if (!f) return R::err(Error::Backend);  // missing / unreadable source
      std::fclose(f);
    }
    // Not every failure can be caught before building: a videoless-but-readable
    // source only reveals itself in PAUSED, by which point a file sink has
    // already created its file. A leftover zero-byte .ts reads as output to
    // anything scanning the directory and silently clobbers whatever was at that
    // path, so failures below unlink it — but only if THIS call created it, so a
    // pre-existing file the caller cared about is never deleted by a failed open.
    // The same path+flag is handed to the Inserter, which extends the guarantee
    // over the rest of the session (a failing finish(), or an abandoned one —
    // ADR 0022).
    std::string sink_path;
    bool sink_preexisted = true;
    if (cfg.sink.rfind("file:", 0) == 0) {
      sink_path = cfg.sink.substr(5);
      std::FILE* f = std::fopen(sink_path.c_str(), "rb");
      sink_preexisted = (f != nullptr);
      if (f) std::fclose(f);
    }
    // Teardown for every failure past pipeline construction. set_state(NULL)
    // closes the sink's file before we unlink it.
    auto fail = [&](GstElement* pipe, Error e) {
      gst_element_set_state(pipe, GST_STATE_NULL);
      gst_object_unref(pipe);
      if (!sink_path.empty() && !sink_preexisted) std::remove(sink_path.c_str());
      return R::err(e);
    };

    GstElement* pipeline = gst_pipeline_new("misbklv-insert");
    GstElement* appsrc = gst_element_factory_make("appsrc", "src");
    GstElement* mux = gst_element_factory_make("mpegtsmux", "mux");
    GstElement* sink = make_sink(cfg.sink);
    if (!pipeline || !appsrc || !mux || !sink) {
      if (sink) gst_object_unref(sink);
      if (mux) gst_object_unref(mux);
      if (appsrc) gst_object_unref(appsrc);
      if (pipeline) gst_object_unref(pipeline);
      return Result<std::unique_ptr<Inserter>>::err(sink ? Error::Backend
                                                         : Error::Unsupported);
    }
    GstCaps* caps = gst_caps_from_string("meta/x-klv, parsed=(boolean)true");
    // realtime: the appsrc is a live source and the sink renders on the clock, so
    // output is paced to the per-buffer PTS (~30 fps) instead of pushed as fast as
    // the sink drains — the real-time streaming behavior (B4). Off = fast, for a
    // filesink round-trip.
    g_object_set(appsrc, "caps", caps, "format", GST_FORMAT_TIME, "block", TRUE,
                 "is-live", cfg.realtime ? TRUE : FALSE, nullptr);
    gst_caps_unref(caps);
    if (cfg.realtime) g_object_set(sink, "sync", TRUE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), appsrc, mux, sink, nullptr);
    if (!gst_element_link_many(appsrc, mux, sink, nullptr))
      return fail(pipeline, Error::Backend);

    // Video passthrough: filesrc ! demuxer, joining the same muxer. The
    // demuxer's pads are linked as they appear, through a parser where the
    // codec needs one, and nothing is ever decoded — which is what keeps this
    // codec-agnostic (H.264 / H.265 alike).
    std::unique_ptr<VideoCtx> video;
    if (!video_path.empty()) {
      const std::string container = sniff_container(video_path);
      const char* demuxer_name = demuxer_for_media_type(container);
      if (!demuxer_name) {
        g_warning("misbklv: video source '%s': %s is not a container this "
                  "library demuxes",
                  video_path.c_str(),
                  container.empty() ? "an unrecognised format" : container.c_str());
        return fail(pipeline, Error::Unsupported);
      }
      GstElement* vsrc = gst_element_factory_make("filesrc", "vsrc");
      GstElement* parse = gst_element_factory_make(demuxer_name, "vparse");
      if (!vsrc || !parse) {
        if (vsrc) gst_object_unref(vsrc);
        if (parse) gst_object_unref(parse);
        g_warning("misbklv: could not create %s — is the plugin providing it "
                  "installed?", demuxer_name);
        return fail(pipeline, Error::Backend);
      }
      g_object_set(vsrc, "location", video_path.c_str(), nullptr);
      video = std::make_unique<VideoCtx>();
      video->mux = mux;
      video->pipeline = pipeline;  // for creating fakesinks in pad callback

      // ST 0604 SEI generation is opt-in (ADR 0024). Preserve — the default —
      // leaves the video elementary stream alone entirely. Even under Generate,
      // an individual frame gets SEI only once a KLV timestamp matches it.
      video->generate_sei = cfg.sei_0604 == Sei0604::Generate;
      if (video->generate_sei) video->h264_parser = gst_h264_nal_parser_new();

      g_signal_connect(parse, "pad-added", G_CALLBACK(on_video_pad_added),
                       video.get());
      g_signal_connect(parse, "no-more-pads", G_CALLBACK(on_video_no_more_pads),
                       video.get());
      gst_bin_add_many(GST_BIN(pipeline), vsrc, parse, nullptr);
      if (!gst_element_link(vsrc, parse)) return fail(pipeline, Error::Backend);
    }

    // The demuxer exposes its pads while prerolling in PAUSED. Wait for the video
    // pad here, before PLAYING, so the muxer's PMT is written with both
    // elementary streams — otherwise a caller pushing KLV immediately could race
    // the video pad and produce a KLV-only PMT. (KLV-only keeps going straight
    // to PLAYING, exactly as before.)
    if (video) {
      if (gst_element_set_state(pipeline, GST_STATE_PAUSED) ==
          GST_STATE_CHANGE_FAILURE)
        return fail(pipeline, Error::Backend);
      GstBus* bus = gst_element_get_bus(pipeline);
      const auto deadline = std::chrono::steady_clock::now() + kVideoPadTimeout;
      bool linked = false;
      int ignored = 0;
      for (;;) {
        {  // woken by pad-added / no-more-pads; short waits so we also see errors
          std::unique_lock<std::mutex> lk(video->mu);
          if (video->cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return video->linked || video->no_more_pads;
              })) {
            linked = video->linked;
            ignored = video->ignored_video_pads;
            break;
          }
        }
        GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        if (msg) {  // not a media file / no demuxer for it
          // The caller only ever sees Error::Unsupported, so surface the detail
          // here rather than dropping it silently (it is also consumed from the
          // bus, so finish() could not report it later).
          GError* err = nullptr;
          gchar* dbg = nullptr;
          gst_message_parse_error(msg, &err, &dbg);
          // Check if this is a "not-linked" error from qtdemux - this can happen
          // transiently during pad linking but doesn't mean the video source is invalid
          const bool is_not_linked = (dbg && std::strstr(dbg, "not-linked"));
          if (!is_not_linked) {
            g_warning("misbklv: video source '%s': %s", video_path.c_str(),
                      err ? err->message : "pipeline error");
            if (err) g_error_free(err);
            g_free(dbg);
            gst_message_unref(msg);
            break;
          }
          // Ignore transient not-linked errors - wait for linked status instead
          if (err) g_error_free(err);
          g_free(dbg);
          gst_message_unref(msg);
        }
        if (std::chrono::steady_clock::now() > deadline) break;
      }
      gst_object_unref(bus);
      if (!linked)  // unparseable source, or no video stream in it
        return fail(pipeline, Error::Unsupported);
      if (ignored)
        g_warning("misbklv: %d extra video stream(s) in '%s' not carried",
                  ignored, video_path.c_str());
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE)
      return fail(pipeline, Error::Backend);
    return R::ok(std::make_unique<GstInserter>(
        pipeline, appsrc, std::move(video),
        sink_preexisted ? std::string() : sink_path));
  }
};

}  // namespace

std::unique_ptr<MediaBackend> make_gst_backend() {
  return std::make_unique<GstBackend>();
}

}  // namespace misbklv
