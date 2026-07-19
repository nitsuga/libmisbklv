// SPDX-License-Identifier: Apache-2.0
// Real-world hardening: inputs the clean vendor samples never exercised.
//  (1) multi-byte BER-OID tags (>=128, e.g. 0601 Item 143 MSID),
//  (2) Report-on-Change trimmed packets (fewer items than a full frame),
//  (3) malformed / adversarial bytes must return a Result error, never crash
//      or invoke UB (run under -fsanitize=address,undefined via MISBKLV_SANITIZE).
// argv: <dayflight_first_packet.klv>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <span>
#include <vector>

#include "misbklv/builder.hpp"
#include "misbklv/codec.hpp"
#include "misbklv/packet.hpp"
#include "misbklv/ts.hpp"
#include "misbklv/types.hpp"
#include "misbklv/registry/uas0601_tables.generated.hpp"

using namespace misbklv;

static int failures = 0;
static void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}

static std::vector<std::byte> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i)
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}
static std::byte B(int v) { return static_cast<std::byte>(v & 0xFF); }

// --- (1) multi-byte BER-OID tag (>=128) -------------------------------------
static void test_multibyte_tag() {
  std::printf("(1) multi-byte BER-OID tag (>=128)\n");
  const std::byte payload[] = {B(0xAB), B(0xCD), B(0xEF)};
  LocalSetBuilder b(gen::uas_0601);
  b.append_raw(143, payload);  // Item 143 MSID — unregistered here, raw is fine
  auto pkt = std::move(b).finalize(std::span{kUas0601Key}, /*enforce=*/false);
  check(static_cast<bool>(pkt), "build packet with tag 143");
  if (!pkt) return;

  // Tag 143 must be on the wire as BER-OID 0x81 0x0F (not a single byte).
  bool found_oid = false;
  for (std::size_t i = 0; i + 1 < pkt->size(); ++i)
    if ((*pkt)[i] == B(0x81) && (*pkt)[i + 1] == B(0x0F)) found_oid = true;
  check(found_oid, "tag 143 encoded as 0x81 0x0F");

  auto parsed = parse_packet(*pkt);
  check(static_cast<bool>(parsed), "re-parse");
  if (!parsed) return;
  bool ok143 = false;
  for (const auto& it : parsed->items)
    if (it.tag == 143 && it.value.size() == 3 && it.value[0] == B(0xAB))
      ok143 = true;
  check(ok143, "tag 143 round-trips with value intact");
}

// --- (2) Report-on-Change trimmed packet ------------------------------------
static void test_roc_trimmed(const std::vector<std::byte>& full_pkt) {
  std::printf("(2) Report-on-Change trimmed packet\n");
  auto full = parse_packet(full_pkt);
  check(static_cast<bool>(full), "parse full reference packet");
  if (!full) return;

  // Build a trimmed frame: keep only tags 2 (timestamp) + 5 + 6 if present.
  LocalSetBuilder b(gen::uas_0601);
  std::size_t kept = 0;
  for (const auto& it : full->items) {
    if (it.tag == 1) continue;                    // checksum re-emitted
    if (it.tag != 2 && it.tag != 5 && it.tag != 6) continue;
    b.append_raw(it.tag, it.value);               // raw preserves exact bytes
    ++kept;
  }
  auto trimmed = std::move(b).finalize(std::span{kUas0601Key}, /*enforce=*/false);
  check(static_cast<bool>(trimmed), "finalize trimmed (mandatory opt-out)");
  if (!trimmed) return;

  auto reparse = parse_packet(*trimmed);
  check(static_cast<bool>(reparse), "parse trimmed packet");
  if (!reparse) return;
  check(reparse->items.size() == kept + 1, "trimmed item count (+checksum)");

  // The kept timestamp must still decode.
  const ItemDescriptor* d2 = gen::uas_0601.find(2);
  bool ts_ok = false;
  for (const auto& it : reparse->items)
    if (it.tag == 2 && d2) ts_ok = static_cast<bool>(codec::decode(*d2, it.value));
  check(ts_ok, "kept item (tag 2) still decodes");

  // Reserialize the trimmed packet byte-exact.
  LocalSetBuilder b2(gen::uas_0601);
  for (const auto& it : reparse->items)
    if (it.tag != 1) b2.append_raw(it.tag, it.value);
  auto again = std::move(b2).finalize(std::span{kUas0601Key}, false);
  check(again && *again == *trimmed, "trimmed packet round-trips byte-exact");
}

// --- (3) malformed / adversarial input --------------------------------------
static void test_malformed() {
  std::printf("(3) malformed input (must error, never crash/UB)\n");

  check(!parse_packet({}), "empty buffer -> error");
  std::vector<std::byte> short_buf(15, B(0));
  check(!parse_packet(short_buf), "15-byte buffer -> error");

  // 8-byte BER length = 0xFFFFFFFFFFFFFFFF: the integer-overflow OOB case.
  std::vector<std::byte> huge;
  for (std::uint8_t k : kUas0601Key) huge.push_back(B(k));
  huge.push_back(B(0x88));                       // long form, 8 length bytes
  for (int i = 0; i < 8; ++i) huge.push_back(B(0xFF));
  check(!parse_packet(huge), "8-byte huge length -> Truncated (no OOB)");
  check(packet_frame_length(huge) == 0, "frame_length rejects huge length");

  // Bare TLV with a huge item length.
  std::vector<std::byte> item{B(0x05), B(0x88)};
  for (int i = 0; i < 8; ++i) item.push_back(B(0xFF));
  check(!parse_items(item), "item with huge length -> Truncated (no OOB)");

  // Zero-length + over-long numeric decode (shift-UB / rd_uint wrap guards).
  const ItemDescriptor* sgn = gen::uas_0601.find(13);   // SensorLatitude, signed
  const ItemDescriptor* u = gen::uas_0601.find(2);       // Precision Time, UInt
  check(sgn && !codec::decode(*sgn, {}), "0-length signed decode -> BadLength");
  std::vector<std::byte> nine(9, B(0));
  check(u && !codec::decode(*u, nine), "9-byte UInt decode -> BadLength");

  // Garbage fed to the TS extractor must not crash.
  std::vector<std::byte> garbage(1000, B(0xA5));
  bool extract_returned = true;  // reaching here at all means no crash/UB
  extract_ts_klv(garbage, [](const KlvPacket&) {});
  check(extract_returned, "extract_ts_klv(garbage) returns without crashing");
}

int main(int argc, char** argv) {
  const char* path =
      argc > 1 ? argv[1] : "test/fixtures/dayflight_first_packet.klv";
  const auto full = read_file(path);
  if (full.empty()) {
    std::fprintf(stderr, "could not read fixture: %s\n", path);
    return 2;
  }
  test_multibyte_tag();
  test_roc_trimmed(full);
  test_malformed();
  std::printf("%s\n", failures == 0 ? "HARDENING: all PASS" : "HARDENING: FAIL");
  return failures == 0 ? 0 : 1;
}
