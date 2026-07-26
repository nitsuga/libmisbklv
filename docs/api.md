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

for (Message& m : in) {
  if (auto lat = m.get<double>(tags::Uas0601::SensorLatitude))
    m.set(tags::Uas0601::SensorLatitude, Value{*lat + 0.001});   // nudge ~100 m north
  out.emit(m);
}
out.close();
```

`for (Message& m : in)` pulls each packet until the source ends (EOS for a file,
idle for a live source). `emit` re-encodes and muxes; `close` drains. See
[`examples/klv_edit.cpp`](../examples/klv_edit.cpp).

## Write video + KLV in one pass

To *author* a stream rather than edit one — video from an existing file, KLV you
generated — give the sink a `video_source`. Its video elementary stream is
re-muxed **unchanged** (parsed, never decoded, so H.264 and H.265 both just
work); the source's audio and any KLV it already carries are dropped.

```cpp
KlvSink out("file:output.ts", /*realtime=*/false, "input.mp4");

for (auto& [pts_ns, msg] : my_klv) {   // your converted metadata
  msg.set_pts(pts_ns);                 // REQUIRED: see the timeline note
  out.emit(msg);
}
out.close();
```

- **One timeline.** The video is timestamped from the start of `input.mp4`, and
  your KLV must be on that same timeline — presentation time from the start of
  the source, in nanoseconds. Not wall-clock, not epoch. A Message with no PTS is
  **rejected** (`Error::Unsupported`) rather than given a synthetic one, because a
  synthetic timestamp drifts silently against real frame timing.
- **Push in order, as the video flows.** The muxer waits on the slower stream and
  `emit` blocks, which is the backpressure working — but don't try to emit a
  whole file's KLV before the video has started.
- `realtime` pacing is not supported with a video source. A source that is
  missing, unreadable, or carries no video stream fails at construction (`emit`
  then errors) and leaves no output file behind.

The same field exists on the lower level: `open_insert({.sink = "file:out.ts",
.video_source = "in.mp4"})`. Leave it empty for the KLV-only pipeline.

## Just a packet (no gstreamer)

`Message` needs no media backend — hand it raw KLV bytes (e.g. from
`extract_ts_klv`, which pulls KLV from a `.ts` buffer with zero dependencies):

```cpp
#include "misbklv/message.hpp"
using namespace misbklv;

auto msg = Message::parse(klv_bytes);       // Result<Message>
if (!msg) return;
double lat  = msg->get<double>(tags::Uas0601::SensorLatitude).value_or(0.0);
auto uas_ts = msg->get<std::uint64_t>(tags::Uas0601::PrecisionTimeStamp);
msg->set(tags::Uas0601::SensorLatitude, Value{lat + 1.0});
auto bytes  = msg->encode();                 // Result<Bytes>; byte-exact if unedited
```

- **`get<T>(tag)`** returns `std::optional<T>`. `T` must match the item's kind:
  `double` for mapped items (lat/lon/angles), `std::uint64_t` for counts/times,
  `std::string_view` for text, `std::span<const std::byte>` for opaque/nested.
  Wrong type or absent tag → `nullopt`.
- **`set(tag, Value)`** stages a typed edit (re-encoded at the item's on-wire
  width). Unknown tag → error.
- **`encode()`** rebuilds the packet: untouched items pass through byte-exact,
  edited ones via the codec, the checksum is recomputed. No edits → identical
  bytes.
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
  `Vtarget0903`, a pack).
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
