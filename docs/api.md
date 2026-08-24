# libmisbklv API guide

Read, edit, and write MISB KLV (ST 0601 / ST 0903) over MPEG-TS. Two layers:

- **`Message`** (core, no gstreamer) — one owned KLV packet: parse an existing
  one, or author a new one; read/edit typed values; encode.
- **`KlvStream` / `KlvSink`** (needs gstreamer) — stream Messages from/to a file
  or a live `udp:` / `srt:` endpoint.

## Read / edit / write a stream

```cpp
#include "misbklv/stream.hpp"
using namespace misbklv;

KlvStream in("input.ts");                 // file, or "udp:127.0.0.1:5004", "srt:..."
KlvSink   out("file:output.ts");          // realtime pacing: KlvSink(sink, true)

if (out.error()) return;                // exact open_insert error

for (Message& m : in) {
  if (auto lat = m.get<double>(tags::Uas0601::SensorLatitude))
    m.set(tags::Uas0601::SensorLatitude, Value{*lat + 0.001});   // nudge ~100 m north
  if (auto sent = out.emit(m); !sent) return;
}
if (in.error()) return;                           // terminal extraction/parse error
if (auto closed = out.close(); !closed) return;
```

`for (Message& m : in)` pulls each packet until the source ends (EOS for a file;
UDP after its idle timeout). SRT has no equivalent idle signal, so it continues
until cancellation (for example, destroying the stream) or external source
termination/error. `emit` re-encodes and muxes; `close` drains. A caller that
needs to interrupt a post-EOS drain can pass a `std::stop_token` to
`close(stop)`. Cooperative cancellation returns success and removes any partial
file output; the default token still drains normally. See
[`examples/klv_edit.cpp`](../examples/klv_edit.cpp) and
[ADR 0032](../context/decisions/0032-cancellable-insert-drain.md).

`KlvStream` accepts `ExtractOptions` to change the 16 MiB default cap on a
complete incrementally reassembled KLV frame. A declared frame above that cap
ends iteration with `Error::ResourceLimit`; already queued valid Messages still
arrive before the terminal `in.error()` check. A Message that cannot be parsed
ends the stream at that packet rather than being skipped. Normal EOS and
cooperative cancellation leave `error()` empty.

`KlvSink` construction stores its exact `open_insert` error for `error()`, and
the same error is returned by `emit()` and `close()`. Check it before sending,
then check every returned `Result` as in the example.

- **Timing carries through.** `m.pts()` is where the packet sat in the source —
  nanoseconds from the start of it — and `emit` writes it back at that time, so
  an edit does not re-time the stream. A source whose KLV carries no PES
  timestamps reports `kNoPts` (`-1`) and the sink falls back to a synthesized
  ~30 fps counter; correlate such a stream by its KLV Item 2 Precision Time
  Stamp instead.
- **No half-written output.** An output file exists only if `close()` succeeded.
  A failing close — or dropping the sink without calling it — removes the file
  the sink created. A file that was already at that path is never deleted (but
  it *is* truncated when the sink opens it, success or not).

## Write video + KLV in one pass

To *author* a stream rather than edit one, give the sink a `video_source`. It can
be a bare/file-prefixed path, an `rtsp://` or `rtsps://` URI, or an explicit
`pipeline:<gst-launch description>` whose bin exposes a static source pad. Its
video elementary stream is re-muxed **unchanged** (parsed, never decoded, so
H.264 and H.265 both just work); the source's audio and any KLV it already
carries are dropped.

**Select streams by index or by type.** The muxer announces the video first, so
`0:0` is the video and `0:1` the KLV (`ffmpeg -map 0:0` or `-map 0:v`). Video is
announced first because its muxer pad is reserved before the KLV `appsrc` links,
giving it the lower PID — see
[ADR 0020](../context/decisions/0020-video-passthrough.md) § Stream order. PIDs
and stream types are correct either way.

```cpp
KlvSink out("file:output.ts", /*realtime=*/false, "input.mp4");

for (auto& [pts_ns, msg] : my_klv) {   // your converted metadata
  msg.set_pts(pts_ns);                 // REQUIRED: see the timeline note
  out.emit(msg);
}
out.close();
```

- **One timeline.** For a file, the video timeline starts at the beginning of
  the source. For RTSP or `pipeline:` live video, it is the branch's running
  time. Your KLV must use that same timeline in nanoseconds — not wall-clock or
  epoch. A Message with no PTS is **rejected** (`Error::Unsupported`) rather
  than given a synthetic one, because a synthetic timestamp drifts silently
  against real frame timing. It is the same timeline `KlvStream` reports, so a
  Message read from a stream is already positioned correctly.
- **Push in order, as the video flows.** The muxer waits on the slower stream and
  `emit` blocks, which is the backpressure working. Do not enqueue metadata far
  ahead of the corresponding video.
- **Realtime video is supported.** With a file source, `realtime=true` replays
  the recording on the pipeline clock. For an RTSP or `pipeline:` live source,
  it is the normal clock-paced configuration. An invalid file, a terminal
  source error, or a live source that exposes no usable video pad within the
  bounded open wait fails without leaving a newly created output file. A parse
  or pipeline error that appears later is reported by `close()` and has the
  same cleanup guarantee.

The same field exists on the lower level: `open_insert({.sink = "file:out.ts",
.video_source = "in.mp4"})`. Leave it empty for the KLV-only pipeline.

### Live multicast output

For a clock-paced multicast sink, pass the full `InsertConfig` so the network
and video-source settings stay named:

```cpp
KlvSink out({.sink              = "udp:239.10.10.10:5004",
             .realtime          = true,
             .video_source      = "rtsp://camera.example/live",
             .udp_ttl_mcast     = 4,
             .udp_mcast_iface   = "eth0",
             .udp_loop          = false});
```

`udp_ttl_mcast` maps to multicast TTL, `udp_mcast_iface` selects the egress
interface, and `udp_loop` controls local multicast loopback. Defaults preserve
GStreamer's normal behavior (TTL 1, automatic interface, loopback enabled).
These fields are ignored by `file:` and `srt:` sinks.

### ST 0604 timestamps in the video stream

ST 0604 puts a Precision Time Stamp in the *video* elementary stream, so a
player can time frames without reading the KLV. By default we don't write one —
your video is carried across exactly as it arrived. Ask for it when a downstream
reader needs it:

```cpp
KlvSink out("file:output.ts", /*realtime=*/false, "input.mp4",
            Sei0604::Generate);

// or, once more than the sink is set, pass the config whole:
KlvSink out({.sink         = "file:output.ts",
             .video_source = "input.mp4",
             .sei_0604     = Sei0604::Generate});
```

- **`Preserve`** (default) — the video elementary stream is untouched, so it
  comes out byte-identical. Whatever ST 0604 the source had, it keeps.
- **`Generate`** — each access unit we can time gets a Precision Time Stamp
  built from your KLV's item 2, injected before the first slice. Your KLV
  becomes the stream's one timestamp authority: any ST 0604 the source carried
  is *replaced*, so a reader never has to choose between two.

A frame with no KLV timestamp within 200 ms gets no ST 0604 rather than a made-up
one — and under `Generate` the source's is removed there too, so provenance
doesn't silently vary frame to frame. If your KLV covers the video, as it
normally does, every frame gets one.

Matching assumes your KLV PTS and the video source share one timeline. When the
video branch contains an encoder that shifts its output onto a DTS-headroom
timeline (`x264enc`, `avenc_h264` — GStreamer's `gst_video_encoder_set_min_pts`),
the matcher detects the mismatch and uses the branch's TIME segment to recover
the source running time, including when that source starts at a non-zero PTS
(ADR 0033).

`Generate` is **H.264 only**: on any other codec `open_insert` / the `KlvSink`
constructor fails with `Error::Unsupported` rather than handing you a file whose
video quietly has no timestamps. `Preserve` carries every codec.

The Time Status byte that rides with each timestamp (ST 0603.5 §7.4) is derived,
not assumed. Bit 7 always says **Lock Unknown** — we are relaying your item 2 and
cannot know how the source clock was disciplined. Bits 6/5 report a
**Discontinuity** when your absolute time stops tracking the media timeline
between packets (and whether it jumped forward or back), so an edit or a clock
relock is visible to a reader instead of being smoothed over.

## Just a packet (no gstreamer)

`Message` needs no media backend — hand it raw KLV bytes (e.g. from
`extract_ts_klv`, which pulls KLV from a `.ts` buffer with zero dependencies and
timestamps each packet the same way `KlvStream` does — it also reads the `0x15`
sync-KLV streams gstreamer's demuxer drops):

```cpp
#include "misbklv/message.hpp"
using namespace misbklv;

auto msg = Message::parse(klv_bytes);       // Result<Message>
if (!msg) return;
double lat  = msg->get<double>(tags::Uas0601::SensorLatitude).value_or(0.0);
auto uas_ts = msg->get<std::uint64_t>(tags::Uas0601::PrecisionTimeStamp);
msg->set(tags::Uas0601::SensorLatitude, Value{lat + 1.0});
auto bytes  = msg->encode();                 // Result<Bytes>; original packet extent if unedited
```

- **`get<T>(tag)`** returns `std::optional<T>`. `T` must match the item's kind:
  `double` for mapped items (lat/lon/angles), `std::uint64_t` for counts/times,
  `std::string_view` for text, `std::span<const std::byte>` for opaque/nested.
  Wrong type or absent tag → `nullopt`.
- **`set(tag, Value)`** stages a typed edit (re-encoded at the item's on-wire
  width). Unknown tag → error.
- **`has(tag)`** reflects both items in the parsed source and staged additions.
- **`encode()`** returns exactly the original packet extent for an unedited
  parsed Message — preserving noncanonical BER and checksum placement while
  excluding trailing input bytes. An edited Message is rebuilt: source items
  without edits pass through byte-exact, edits and additions use the codec, and
  the checksum is recomputed.
- **Named tags** — `tag` may be a plain number or a generated per-registry enum:
  `tags::Uas0601::SensorLatitude`, `tags::Vmti0903::…`, `tags::Vtarget0903::…`
  (generated from the registry, so the value equals the ST tag number). Names and
  numbers are interchangeable; use a number for any tag not in the registry.

The registry (ST 0601 vs standalone ST 0903 VMTI) is chosen automatically from
the packet's 16-byte UL key.

## Create a packet from scratch

`Message::create` starts an empty packet for a registry; populate it with the
same typed `set()`, then `encode()` (or hand it to a `KlvSink`):

```cpp
#include "misbklv/message.hpp"
using namespace misbklv;

auto msg = Message::create(RegistryId::Uas0601);   // ST 0601; or Vmti0903 (standalone VMTI)
if (!msg) return;
msg->set(tags::Uas0601::PrecisionTimeStamp, Value{std::uint64_t{ts}});  // set it first
msg->set(tags::Uas0601::SensorLatitude,  Value{54.68});    // double, mapped
msg->set(tags::Uas0601::SensorLongitude, Value{-110.17});
auto bytes = msg->encode();      // Result<Bytes>: UL key + items + checksum (auto)

// ...emit it live exactly like an edited one:
// KlvSink out("file:out.ts"); out.emit(*msg); out.close();
```

- **`create(registry)`** takes a standalone packet type — `RegistryId::Uas0601`
  or `RegistryId::Vmti0903`. It errors for a non-standalone type (e.g.
  `Vtarget0903`, a pack), and `encode()` builds a packet from its staged items.
- Items are emitted **in the order you `set()` them**, so set Item 2 (Precision
  Time Stamp) first for ST 0601. The checksum (Item 1) is appended automatically
  last; `get<T>` reads back whatever you've set.
- For advanced authoring — **nested** Local Sets (e.g. a VMTI LS inside 0601
  Item 74), **VTarget Series**, mandatory-item enforcement, or a custom UL key —
  drop to the lower-level [`LocalSetBuilder`](../include/misbklv/builder.hpp) and
  [`series.hpp`](../include/misbklv/series.hpp) that `Message` is built on.

## Build

```cmake
# Core only (Message, parse, codec) — no gstreamer dependency:
find_package(misbklv REQUIRED)
target_link_libraries(app PRIVATE misbklv::misbklv)

# ...or with the streaming facade (KlvStream / KlvSink), which needs gstreamer:
find_package(misbklv REQUIRED COMPONENTS gst)
target_link_libraries(app PRIVATE misbklv::gst)   # transitively brings in the core
```

`gst` is an opt-in component so core-only consumers don't inherit the gstreamer
dependency; its config re-discovers gstreamer (≥ 1.20). The core is
dependency-free.
