# libmisbklv API guide

Read, edit, and write MISB KLV (ST 0601 / ST 0903) over MPEG-TS. Two layers:

- **`Message`** (core, no gstreamer) — one owned, editable KLV packet.
- **`KlvStream` / `KlvSink`** (needs gstreamer) — stream Messages from/to a file
  or a live `udp:` / `srt:` endpoint.

## Read / edit / write a stream

```cpp
#include "misbklv/stream.hpp"
using namespace misbklv;

KlvStream in("input.ts");                 // file, or "udp:127.0.0.1:5004", "srt:..."
KlvSink   out("file:output.ts");          // realtime pacing: KlvSink(sink, true)

for (Message& m : in) {
  if (auto lat = m.get<double>(13))       // ST 0601 tag 13 = Sensor Latitude
    m.set(13, Value{*lat + 0.001});       // nudge ~100 m north
  out.emit(m);
}
out.close();
```

`for (Message& m : in)` pulls each packet until the source ends (EOS for a file,
idle for a live source). `emit` re-encodes and muxes; `close` drains. See
[`examples/klv_edit.cpp`](../examples/klv_edit.cpp).

## Just a packet (no gstreamer)

`Message` needs no media backend — hand it raw KLV bytes (e.g. from
`extract_ts_klv`, which pulls KLV from a `.ts` buffer with zero dependencies):

```cpp
#include "misbklv/message.hpp"

auto msg = Message::parse(klv_bytes);       // Result<Message>
if (!msg) return;
double lat = msg->get<double>(13).value_or(0.0);
auto uas_ts = msg->get<std::uint64_t>(2);   // Precision Time Stamp
msg->set(13, Value{lat + 1.0});
auto bytes = msg->encode();                  // Result<Bytes>; byte-exact if unedited
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

The registry (ST 0601 vs standalone ST 0903 VMTI) is chosen automatically from
the packet's 16-byte UL key.

## Build

```cmake
find_package(misbklv REQUIRED)
target_link_libraries(app PRIVATE misbklv::misbklv)   # core: Message, parse, codec
target_link_libraries(app PRIVATE misbklv::gst)       # + KlvStream / KlvSink
```

`misbklv::gst` is built only when gstreamer (≥ 1.20) is present. The core is
dependency-free.
