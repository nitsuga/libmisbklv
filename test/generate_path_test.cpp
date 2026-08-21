// SPDX-License-Identifier: Apache-2.0
// Issue #26: bounded pts_to_sensor_timestamp map and codec latch.
// Direct unit coverage via internal header seam (like udp_multicast_test).
// Updated for consumer-side eviction: the map is bounded by video consumption,
// not by producer prune. Includes lagging-video regression.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <gst/gst.h>

#include "gst_backend_internal.hpp"
#include "misbklv/backend.hpp"
#include "misbklv/gst_backend.hpp"
#include "misbklv/message.hpp"
#include "misbklv/packet.hpp"

using namespace misbklv;
using namespace misbklv::detail;

static int failures = 0;
static void check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  } else {
    std::printf("PASS: %s\n", what);
  }
}

static std::vector<std::byte> make_packet(std::uint64_t ts_us) {
  auto msg = Message::create(RegistryId::Uas0601);
  if (!msg) return {};
  (void)msg->set(2, Value{ts_us});
  (void)msg->set(65, Value{std::uint64_t{19}});
  auto enc = msg->encode();
  if (!enc) return {};
  return std::vector<std::byte>(enc->begin(), enc->end());
}

inline constexpr std::uint64_t kTolNs = 200'000'000;

// Map-level lagging regression: KLV pushed >1 s ahead must not be pruned before
// the late frame that needs it. Old producer prune (1 s window) would delete it.
static void test_lagging_map_regression() {
  std::printf("== lagging-video map regression: KLV >1 s ahead still matched ==\n");
  VideoCtx ctx;
  // Two KLV pushes: one at 0, one at 2.5 s (2.5 s lead, >1 s window).
  auto pkt0 = make_packet(1'600'000'000'000'000ULL);
  auto pkt1 = make_packet(1'600'000'002'500'000ULL);
  record_sensor_timestamp(ctx, pkt0, 0);
  record_sensor_timestamp(ctx, pkt1, static_cast<std::int64_t>(2'500'000'000LL));
  {
    std::lock_guard<std::mutex> lk(ctx.timestamp_mu);
    // With producer prune removed, both entries must still be present.
    check(ctx.pts_to_sensor_timestamp.size() == 2, "both KLV entries retained (no producer prune)");
    auto f = ctx.pts_to_sensor_timestamp.find(0);
    check(f != ctx.pts_to_sensor_timestamp.end(), "entry at pts 0 not pruned despite 2.5 s lead");
  }
  // Simulate late frame at pts 0 arriving after the 2.5 s push.
  // The lookup should still find the pts 0 entry.
  bool matched = false;
  std::uint64_t matched_ts = 0;
  {
    std::lock_guard<std::mutex> lk(ctx.timestamp_mu);
    const std::uint64_t frame_pts = 0;
    auto it = ctx.pts_to_sensor_timestamp.upper_bound(frame_pts);
    if (it != ctx.pts_to_sensor_timestamp.begin()) {
      --it;
      if (frame_pts >= it->first && frame_pts - it->first <= kTolNs) {
        matched = true;
        matched_ts = it->second.timestamp_us;
      }
    }
    // No consumer eviction at pts 0 (threshold would be negative), so size stays 2.
  }
  check(matched, "late frame at pts 0 still matched after 2.5 s lead");
  if (matched) check(matched_ts == 1'600'000'000'000'000ULL, "matched timestamp is the early KLV");
  // Now simulate consumption advancing to 2.5 s — this should prune the old entry.
  {
    std::lock_guard<std::mutex> lk(ctx.timestamp_mu);
    const std::uint64_t frame_pts = 2'500'000'000ULL;
    if (frame_pts >= kTolNs) {
      const std::uint64_t thr = frame_pts - kTolNs;
      auto it = ctx.pts_to_sensor_timestamp.begin();
      while (it != ctx.pts_to_sensor_timestamp.end() && it->first < thr)
        it = ctx.pts_to_sensor_timestamp.erase(it);
    }
    check(ctx.pts_to_sensor_timestamp.size() == 1, "consumer eviction after 2.5 s frame leaves 1 entry");
    check(ctx.pts_to_sensor_timestamp.find(0) == ctx.pts_to_sensor_timestamp.end(), "entry at 0 evicted after consumption passes tolerance");
  }
  // Demonstrate old producer prune would have failed: simulate its logic.
  {
    // Recreate the old window: 1 s = 5 * tolerance.
    constexpr std::uint64_t kPruneWindowNs = kTolNs * 5;
    std::map<std::uint64_t, SensorTime> old_map;
    old_map[0] = SensorTime{1'600'000'000'000'000ULL, 0x9F};
    const std::uint64_t new_pts = 2'500'000'000ULL;
    old_map[new_pts] = SensorTime{1'600'000'002'500'000ULL, 0x9F};
    while (!old_map.empty() && old_map.begin()->first + kPruneWindowNs < new_pts)
      old_map.erase(old_map.begin());
    check(old_map.find(0) == old_map.end(), "old producer prune would have evicted pts 0 (demonstrates bug)");
  }
}

static void test_bounded_map_with_consumption() {
  std::printf("== bounded-map: bounded by consumption, not producer prune ==\n");
  VideoCtx ctx;
  constexpr int kCount = 10000;
  constexpr std::int64_t kStepNs = 33'333'333;
  // Phase 1: push without consumption — map grows without bound (producer no longer prunes).
  for (int i = 0; i < kCount; ++i) {
    auto pkt = make_packet(1'600'000'000'000'000ULL + static_cast<std::uint64_t>(i) * 33'333);
    if (pkt.empty()) { check(false, "packet encode"); return; }
    record_sensor_timestamp(ctx, pkt, static_cast<std::int64_t>(i) * kStepNs);
  }
  {
    std::lock_guard<std::mutex> lk(ctx.timestamp_mu);
    size_t sz = ctx.pts_to_sensor_timestamp.size();
    std::printf("  after %d pushes without consumption: size %zu (expected %d, no producer bound)\n", kCount, sz, kCount);
    check(sz == static_cast<size_t>(kCount), "map grows to push count when video never arrives (inherent)");
  }
  // Phase 2: simulate video consumption catching up to the far-ahead KLV.
  // While video lags, the map stays large (bounded by lead, not just tolerance);
  // after consumption finishes, only the tolerance window remains.
  {
    std::lock_guard<std::mutex> lk(ctx.timestamp_mu);
    for (int i = 0; i < kCount; ++i) {
      const std::uint64_t frame_pts = static_cast<std::uint64_t>(i) * static_cast<std::uint64_t>(kStepNs);
      if (frame_pts >= kTolNs) {
        const std::uint64_t thr = frame_pts - kTolNs;
        auto it = ctx.pts_to_sensor_timestamp.begin();
        while (it != ctx.pts_to_sensor_timestamp.end() && it->first < thr)
          it = ctx.pts_to_sensor_timestamp.erase(it);
      }
    }
    size_t sz = ctx.pts_to_sensor_timestamp.size();
    std::printf("  after consuming %d frames: size %zu (bounded by tolerance window)\n", kCount, sz);
    // Tolerance 200 ms / 33 ms ≈ 6 frames, plus one for the current
    check(sz <= 10, "final size bounded by consumption (tolerance window)");
    check(sz > 0, "map not empty after consumption");
  }
  // Phase 3: interleaved push+consume stays bounded throughout (the normal case).
  {
    VideoCtx ctx2;
    for (int i = 0; i < kCount; ++i) {
      auto pkt = make_packet(1'600'000'000'000'000ULL + static_cast<std::uint64_t>(i) * 33'333);
      record_sensor_timestamp(ctx2, pkt, static_cast<std::int64_t>(i) * kStepNs);
      // Immediately consume frame i (no lag)
      {
        std::lock_guard<std::mutex> lk(ctx2.timestamp_mu);
        const std::uint64_t frame_pts = static_cast<std::uint64_t>(i) * static_cast<std::uint64_t>(kStepNs);
        if (frame_pts >= kTolNs) {
          const std::uint64_t thr = frame_pts - kTolNs;
          auto it = ctx2.pts_to_sensor_timestamp.begin();
          while (it != ctx2.pts_to_sensor_timestamp.end() && it->first < thr)
            it = ctx2.pts_to_sensor_timestamp.erase(it);
        }
      }
      if ((i + 1) % 2000 == 0) {
        std::lock_guard<std::mutex> lk(ctx2.timestamp_mu);
        size_t sz = ctx2.pts_to_sensor_timestamp.size();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "interleaved bounded after %d (size %zu <=10)", i + 1, sz);
        check(sz <= 10, buf);
      }
    }
    std::lock_guard<std::mutex> lk(ctx2.timestamp_mu);
    std::printf("  interleaved final size %zu\n", ctx2.pts_to_sensor_timestamp.size());
    check(ctx2.pts_to_sensor_timestamp.size() <= 10, "interleaved final bounded");
  }
}

static void test_codec_latch_initial() {
  std::printf("== codec latch: initial Unknown ==\n");
  VideoCtx ctx;
  check(ctx.codec_latch.load() == CodecLatch::Unknown, "initial latch Unknown");
  ctx.codec_latch.store(CodecLatch::IsH264);
  check(ctx.codec_latch.load() == CodecLatch::IsH264, "latch IsH264");
  ctx.codec_latch.store(CodecLatch::NotH264);
  check(ctx.codec_latch.load() == CodecLatch::NotH264, "latch NotH264");
  ctx.codec_latch.store(CodecLatch::IsH264);
  check(ctx.codec_latch.load() == CodecLatch::IsH264, "latch renegotiation to IsH264");
}

// ---- Pipeline lagging-video hermetic test ----
static void test_lagging_pipeline_one_encoder(const char* enc);

static void test_lagging_pipeline_hermetic() {
  gst_init(nullptr,nullptr);
  // Exercise every H.264 encoder present, not just the first pick — see the
  // comment on test_lagging_pipeline_one_encoder.
  bool any = false;
  for (const char* name : {"x264enc", "openh264enc", "avenc_h264"}) {
    GstElementFactory* f = gst_element_factory_find(name);
    if (!f) continue;
    gst_object_unref(f);
    if (any) std::printf("  --\n");
    any = true;
    test_lagging_pipeline_one_encoder(name);
  }
  if (!any) {
    std::printf("== lagging-video pipeline hermetic ==\n");
    std::printf("  SKIP: no H.264 encoder\n");
  }
}

static std::vector<std::byte> read_file_bytes(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (size_t i=0;i<raw.size();++i) out[i]=static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}

constexpr std::size_t kTsPkt = 188;
inline unsigned u8_ts(std::span<const std::byte> b, std::size_t i) { return static_cast<unsigned>(b[i]); }

static std::vector<std::byte> extract_video_payload(std::span<const std::byte> ts, unsigned want_pid) {
  std::vector<std::byte> out;
  std::vector<std::byte> cur;
  for (std::size_t off = 0; off + kTsPkt <= ts.size(); off += kTsPkt) {
    auto p = ts.subspan(off, kTsPkt);
    if (u8_ts(p,0)!=0x47) continue;
    unsigned pid = ((u8_ts(p,1)&0x1f)<<8)|u8_ts(p,2);
    if (pid != want_pid) continue;
    unsigned afc = (u8_ts(p,3)>>4)&0x3;
    if (!(afc & 0x1)) continue;
    std::size_t i=4;
    if (afc & 0x2) i += 1 + u8_ts(p,4);
    if (i>=kTsPkt) continue;
    bool pusi = (u8_ts(p,1)&0x40)!=0;
    if (pusi && cur.size()>=9) {
      std::size_t hdr = 9 + u8_ts(cur,8);
      if (hdr <= cur.size()) out.insert(out.end(), cur.begin()+hdr, cur.end());
      cur.clear();
    }
    // payload start
    cur.insert(cur.end(), p.begin()+i, p.end());
  }
  if (cur.size()>=9) {
    // Handle last PES: need to skip header if present
    if (cur.size()>=4 && u8_ts(cur,0)==0x00 && u8_ts(cur,1)==0x00 && u8_ts(cur,2)==0x01) {
      std::size_t hdr = 9 + u8_ts(cur,8);
      if (hdr < cur.size()) out.insert(out.end(), cur.begin()+hdr, cur.end());
    } else {
      out.insert(out.end(), cur.begin(), cur.end());
    }
  }
  return out;
}

struct EsInfo { unsigned pid=0; unsigned stream_type=0; bool klva=false; };
static std::vector<EsInfo> read_pmt(std::span<const std::byte> ts) {
  std::vector<EsInfo> out;
  unsigned pmt_pid=0;
  for (std::size_t off=0; off+kTsPkt<=ts.size(); off+=kTsPkt) {
    auto p=ts.subspan(off,kTsPkt);
    if (u8_ts(p,0)!=0x47) continue;
    bool pusi=(u8_ts(p,1)&0x40)!=0;
    unsigned pid=((u8_ts(p,1)&0x1f)<<8)|u8_ts(p,2);
    unsigned afc=(u8_ts(p,3)>>4)&0x3;
    if (!(afc & 0x1) || !pusi) continue;
    std::size_t i=4;
    if (afc & 0x2) i+=1+u8_ts(p,4);
    if (i>=kTsPkt) continue;
    i+=1+u8_ts(p,i);
    if (i+8>=kTsPkt) continue;
    unsigned table_id=u8_ts(p,i);
    std::size_t seclen=((u8_ts(p,i+1)&0x0f)<<8)|u8_ts(p,i+2);
    std::size_t end=i+3+seclen-4;
    if (end>kTsPkt) continue;
    if (table_id==0x00 && pmt_pid==0) {
      for (std::size_t j=i+8;j+4<=end;j+=4) {
        unsigned prog=(u8_ts(p,j)<<8)|u8_ts(p,j+1);
        if (prog!=0){pmt_pid=((u8_ts(p,j+2)&0x1f)<<8)|u8_ts(p,j+3);break;}
      }
    } else if (table_id==0x02 && pid==pmt_pid && out.empty()) {
      std::size_t j=i+8+2;
      std::size_t pil=((u8_ts(p,j)&0x0f)<<8)|u8_ts(p,j+1);
      j+=2+pil;
      while (j+5<=end){
        EsInfo es; es.stream_type=u8_ts(p,j); es.pid=((u8_ts(p,j+1)&0x1f)<<8)|u8_ts(p,j+2);
        std::size_t esil=((u8_ts(p,j+3)&0x0f)<<8)|u8_ts(p,j+4);
        for(std::size_t d=j+5;d+2<=j+5+esil;){
          unsigned tag=u8_ts(p,d), len=u8_ts(p,d+1);
          if(tag==0x05 && len>=4 && u8_ts(p,d+2)=='K'&&u8_ts(p,d+3)=='L'&&u8_ts(p,d+4)=='V'&&u8_ts(p,d+5)=='A') es.klva=true;
          d+=2+len;
        }
        out.push_back(es);
        j+=5+esil;
      }
    }
    if(pmt_pid&&!out.empty()) break;
  }
  return out;
}

static std::size_t count_sei(const std::vector<std::byte>& payload) {
  const char kId[]="MISPmicrosectime";
  constexpr std::size_t kIdLen=16;
  std::size_t cnt=0;
  for(std::size_t i=0;i+kIdLen+12<=payload.size();++i){
    bool hit=true;
    for(std::size_t k=0;k<kIdLen&&hit;++k) hit=static_cast<unsigned>(payload[i+k])==static_cast<unsigned>(kId[k]);
    if(!hit) continue;
    if(u8_ts(payload,i+kIdLen+1+2)!=0xFF || u8_ts(payload,i+kIdLen+1+5)!=0xFF || u8_ts(payload,i+kIdLen+1+8)!=0xFF) continue;
    ++cnt;
    i+=kIdLen+12-1;
  }
  return cnt;
}

// One hermetic 90-frame Generate run against a named encoder. Encoders differ
// in output-timeline behavior (x264enc/avenc_h264 shift their whole output by
// the gst_video_encoder_set_min_pts DTS headroom; openh264enc does not —
// ADR 0033), so a single-encoder test is blind to whichever encoder is not
// installed. The caller enumerates every encoder present.
static void test_lagging_pipeline_one_encoder(const char* enc) {
  // Start at 3 s so this catches treating set_min_pts as a fixed offset. The
  // encoder adjustment is min_pts - first_input_pts; the matching KLV remains
  // on the source's 3 s running-time timeline.
  const std::string desc = "pipeline:videotestsrc num-buffers=90 timestamp-offset=3000000000 ! videoconvert ! video/x-raw,width=320,height=240,framerate=30/1 ! " + std::string(enc) + " ! h264parse";
  const std::string label = std::string("[") + enc + "] lagging pipeline";
  const std::string out = "/tmp/generate_lagging_test.ts";
  std::filesystem::remove(out);
  auto be = make_gst_backend();
  auto ins_res = be->open_insert({ "file:" + out, true, desc, Sei0604::Generate });
  if (!ins_res) {
    check(false, (label + " open_insert").c_str());
    std::printf("  %s: open_insert failed %d\n", label.c_str(), (int)ins_res.error());
    return;
  }
  auto& ins = *ins_res;
  // Push 90 KLV packets in a tight burst: each pts 33 ms apart, sensor ts aligned.
  // This creates >1 s KLV-vs-video lead immediately (video is still near 3 s).
  constexpr std::uint64_t kBaseUs = 1'600'000'000'000'000ULL;
  constexpr std::int64_t kPtsBaseNs = 3'000'000'000;
  constexpr std::int64_t kStepNs = 33'333'333;
  const int kFrames = 90;
  std::vector<std::vector<std::byte>> pkts;
  for (int i=0;i<kFrames;++i){
    std::uint64_t ts = kBaseUs + static_cast<std::uint64_t>(i) * 33'333;
    pkts.push_back(make_packet(ts));
  }
  for (int i=0;i<kFrames;++i){
    std::int64_t pts = kPtsBaseNs + static_cast<std::int64_t>(i) * kStepNs;
    auto r = ins->push(pkts[i], pts);
    if (!r) { check(false, (label + " push").c_str()); std::printf(" push %d failed %d\n", i, (int)r.error()); break; }
    // Small delay to let video advance but still keep >1 s lead for future pushes.
    if (i % 10 == 0) g_usleep(5000); // 5 ms per 10 packets
  }
  auto fr = ins->finish();
  check(static_cast<bool>(fr), (label + " finish ok").c_str());
  if (!fr) { std::printf(" finish failed %d\n", (int)fr.error()); std::filesystem::remove(out); return; }
  auto bytes = read_file_bytes(out.c_str());
  std::printf("  output %zu bytes\n", bytes.size());
  auto es = read_pmt(bytes);
  unsigned vpid=0, vtype=0;
  for(auto &e: es){ if(e.stream_type==0x1B) {vpid=e.pid; vtype=e.stream_type;} }
  check(vpid!=0, (label + " video PID present").c_str());
  if (!vpid) { std::filesystem::remove(out); return; }
  std::printf("  video pid 0x%04x type 0x%02x\n", vpid, vtype);
  auto vpayload = extract_video_payload(bytes, vpid);
  std::size_t sei_cnt = count_sei(vpayload);
  std::printf("  SEI count %zu for %d frames\n", sei_cnt, kFrames);
  // Every frame should have an SEI when KLV is provided for every frame and tolerance is 200 ms.
  // With old producer prune, early frames would lose their SEI, so sei_cnt < kFrames.
  check(sei_cnt == static_cast<std::size_t>(kFrames), (label + " every frame got SEI (no silent loss)").c_str());
  if (sei_cnt != static_cast<std::size_t>(kFrames)) {
    std::fprintf(stderr, "  expected %d SEI, got %zu — indicates KLV pruned before use or a timeline mismatch (the bugs this test guards)\n", kFrames, sei_cnt);
  }
  // Also verify KLV byte-exact via extract
  std::vector<std::byte> back;
  auto er = be->extract(out, [&](const KlvPacket& kp){ back.insert(back.end(), kp.bytes.begin(), kp.bytes.end()); });
  check(static_cast<bool>(er), (label + " extract ok").c_str());
  std::vector<std::byte> sent;
  for(auto &p: pkts) sent.insert(sent.end(), p.begin(), p.end());
  if (back.size() != sent.size()) std::fprintf(stderr, "  KLV size mismatch sent %zu back %zu\n", sent.size(), back.size());
  if (back != sent) {
    size_t n = std::min(back.size(), sent.size());
    size_t diff = n;
    for (size_t i=0;i<n;++i) if (back[i]!=sent[i]) { diff=i; break; }
    if (diff<n) std::fprintf(stderr, "  first diff at byte %zu\n", diff);
  }
  check(back == sent, "lagging pipeline KLV byte-exact");
  std::filesystem::remove(out);
}

int main() {
  test_bounded_map_with_consumption();
  test_lagging_map_regression();
  test_codec_latch_initial();
  test_lagging_pipeline_hermetic();
  std::printf("\nGENERATE_PATH: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
