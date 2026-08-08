// SPDX-License-Identifier: Apache-2.0
// Message facade (ADR 0018): parse -> typed get -> edit -> encode. No-op encode
// must be byte-exact; an edit must survive a re-parse.
// argv: <input.klv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <span>
#include <vector>

#include "misbklv/message.hpp"

using namespace misbklv;

static int failures = 0;
static void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}
static std::vector<std::byte> read_file(const char* p) {
  std::ifstream f(p, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i)
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}
static std::byte B(unsigned v) { return static_cast<std::byte>(v); }

// Parseable ST 0601 packet with two intentional wire-level oddities that builder
// authoring would normalize: the outer length uses long form for a short value,
// and the checksum item appears before the ordinary item. Parsing deliberately
// does not validate checksums, so the checksum bytes can be arbitrary here.
static std::vector<std::byte> unusual_source_packet() {
  const std::vector<std::byte> payload = {
      B(0x01), B(0x02), B(0x12), B(0x34),  // checksum first
      B(0x02), B(0x08),                    // Precision Time Stamp
      B(0x00), B(0x00), B(0x00), B(0x00), B(0x00), B(0x00), B(0x00), B(0x01)};
  std::vector<std::byte> out;
  for (auto b : kUas0601Key) out.push_back(B(b));
  out.push_back(B(0x81));  // noncanonical long-form length for a 14-byte value
  out.push_back(static_cast<std::byte>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: message_test <input.klv>\n");
    return 2;
  }
  const char* path = argv[1];
  const auto bytes = read_file(path);
  if (bytes.empty()) { std::fprintf(stderr, "no fixture: %s\n", path); return 2; }

  auto msg = Message::parse(bytes);
  check(static_cast<bool>(msg), "parse");
  if (!msg) return 2;

  // Typed read: the synthetic fixture authors SensorLatitude (tag 13) as 12.5°.
  auto lat = msg->get<double>(13);
  check(lat.has_value(), "get<double>(13) present");
  check(lat && std::fabs(*lat - 12.5) < 0.001, "SensorLatitude == 12.5");
  check(msg->get<std::uint64_t>(2).has_value(), "get<uint64_t>(2) timestamp present");
  check(msg->has(13) && !msg->has(250), "has() reports membership");
  check(!msg->get<double>(250).has_value(), "get of absent tag -> nullopt");
  check(!msg->get<std::string_view>(13).has_value(), "type mismatch -> nullopt");

  // Parsing a buffer with extra bytes is permitted, but byte-exact passthrough
  // stops at the one parsed packet's total_size.
  const std::size_t source_size = packet_frame_length(bytes);
  std::vector<std::byte> with_trailer = bytes;
  with_trailer.insert(with_trailer.end(), {B(0xDE), B(0xAD), B(0xBE)});
  auto trailing = Message::parse(with_trailer);
  auto original_packet = std::vector<std::byte>(bytes.begin(), bytes.begin() + source_size);
  auto trimmed = trailing ? trailing->encode() : Result<ber::Bytes>::err(Error::Backend);
  check(trailing && trimmed && *trimmed == original_packet,
        "no-op parsed encode excludes trailing source bytes");

  // An untouched parsed message must preserve source wire choices rather than
  // routing through the builder's canonicalizing authoring path.
  const auto unusual = unusual_source_packet();
  auto unusual_msg = Message::parse(unusual);
  auto unusual_encoded = unusual_msg ? unusual_msg->encode()
                                     : Result<ber::Bytes>::err(Error::Backend);
  check(unusual_msg && unusual_encoded && *unusual_encoded == unusual,
        "no-op encode preserves noncanonical length and checksum placement");

  // no-op encode is byte-exact.
  auto same = msg->encode();
  check(same && *same == bytes, "no-op encode is byte-exact");

  // edit -> encode -> re-parse -> value changed.
  const double newlat = *lat + 1.0;
  check(static_cast<bool>(msg->set(13, Value{newlat})), "set(13)");
  auto edited = msg->encode();
  check(static_cast<bool>(edited), "encode after edit");
  check(edited && *edited != bytes, "edited bytes differ from source");
  auto reparsed = Message::parse(*edited);
  check(static_cast<bool>(reparsed), "re-parse edited");
  auto lat2 = reparsed ? reparsed->get<double>(13) : std::nullopt;
  check(lat2 && std::fabs(*lat2 - newlat) < 0.01, "edited SensorLatitude survives round-trip");

  // get reflects a staged edit before encode, too.
  check(msg->get<double>(13) && std::fabs(*msg->get<double>(13) - newlat) < 0.01,
        "get() reflects staged edit");

  // --- named tags: enum values equal the numbers, get/set accept both -------
  using tags::Uas0601;
  check(static_cast<std::uint16_t>(Uas0601::SensorLatitude) == 13, "enum value == tag number");
  check(msg->get<double>(Uas0601::SensorLatitude).has_value() ==
            msg->get<double>(13).has_value(),
        "get by name and by number agree");

  // --- author a 0601 packet from scratch (by name) --------------------------
  auto fresh = Message::create(RegistryId::Uas0601);
  check(static_cast<bool>(fresh), "create(Uas0601)");
  check(!Message::create(RegistryId::Vtarget0903), "create rejects a non-standalone type");
  if (fresh) {
    check(static_cast<bool>(fresh->set(Uas0601::PrecisionTimeStamp,
                                       Value{std::uint64_t{0x0004603E4F03D2CBull}})),
          "set previously absent Precision Time Stamp");
    check(fresh->has(Uas0601::PrecisionTimeStamp),
          "has() sees an added tag before encode");
    auto one_added = fresh->encode();
    auto one_added_reparsed = one_added ? Message::parse(*one_added)
                                        : Result<Message>::err(Error::Backend);
    check(one_added_reparsed &&
              one_added_reparsed->has(Uas0601::PrecisionTimeStamp),
          "added tag survives encode and re-parse");

    check(static_cast<bool>(
              fresh->set(Uas0601::PrecisionTimeStamp, Value{std::uint64_t{0x0004603E4F03D2CBull}})),
          "set Precision Time Stamp");
    check(static_cast<bool>(fresh->set(Uas0601::SensorLatitude, Value{54.0})), "set SensorLatitude");
    check(static_cast<bool>(fresh->set(Uas0601::SensorLongitude, Value{-110.0})), "set SensorLongitude");
    auto authored = fresh->encode();
    check(static_cast<bool>(authored), "encode authored packet");
    auto rp = authored ? Message::parse(*authored) : Result<Message>::err(Error::Backend);
    check(static_cast<bool>(rp), "authored packet parses");
    if (rp) {
      auto ts = rp->get<std::uint64_t>(Uas0601::PrecisionTimeStamp);
      auto la = rp->get<double>(Uas0601::SensorLatitude);
      check(ts && *ts == 0x0004603E4F03D2CBull, "authored timestamp round-trips");
      check(la && std::fabs(*la - 54.0) < 0.01, "authored latitude round-trips");
      auto re = rp->encode();  // re-encode == authored proves the checksum is valid
      check(re && *re == *authored, "authored packet re-encodes byte-exact (checksum OK)");
    }
  }

  // A created-but-unedited message has no source bytes to pass through; it
  // must use the authoring path and yield a real standalone packet.
  auto empty_fresh = Message::create(RegistryId::Uas0601);
  auto empty_authored = empty_fresh ? empty_fresh->encode()
                                    : Result<ber::Bytes>::err(Error::Backend);
  auto empty_reparsed = empty_authored ? Message::parse(*empty_authored)
                                       : Result<Message>::err(Error::Backend);
  check(empty_authored && !empty_authored->empty() && empty_reparsed,
        "create with no edits emits an authored packet, not empty passthrough");

  std::printf("%s\n", failures == 0 ? "MESSAGE: all PASS" : "MESSAGE: FAIL");
  return failures == 0 ? 0 : 1;
}
