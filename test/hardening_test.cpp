// SPDX-License-Identifier: Apache-2.0
// Real-world hardening: inputs the clean vendor samples never exercised.
//  (1) multi-byte BER-OID tags (>=128, e.g. 0601 Item 143 MSID),
//  (2) Report-on-Change trimmed packets (fewer items than a full frame),
//  (3) malformed / adversarial bytes must return a Result error, never crash
//      or invoke UB (run under -fsanitize=address,undefined via MISBKLV_SANITIZE).
// argv: <dayflight_first_packet.klv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <limits>
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

static ItemDescriptor numeric_descriptor(ValueKind kind, bool is_signed = false,
                                         MappingParams map = {0.0, 100.0}) {
  ItemDescriptor d{};
  d.kind = kind;
  d.is_signed = is_signed;
  d.map = map;
  return d;
}

template <class F>
static void check_type_mismatch_no_throw(F&& f, const char* what) {
  bool threw = false;
  bool type_mismatch = false;
  try {
    auto r = f();
    type_mismatch = !r && r.error() == Error::TypeMismatch;
  } catch (...) {
    threw = true;
  }
  check(!threw && type_mismatch, what);
}

static void check_bad_numeric_lengths(const ItemDescriptor& d, const Value& v,
                                      const char* what) {
  auto zero = codec::encode(d, v, 0);
  auto nine = codec::encode(d, v, 9);
  check(!zero && zero.error() == Error::BadLength, what);
  check(!nine && nine.error() == Error::BadLength, what);
}

static void check_encoded_bytes(const Result<ber::Bytes>& got,
                                std::initializer_list<std::byte> want,
                                const char* what) {
  check(got && *got == ber::Bytes(want), what);
}

// --- (0) typed encode input validation -------------------------------------
static void test_typed_encode_validation() {
  std::printf("(0) typed encode input validation\n");

  const ItemDescriptor uint_d = numeric_descriptor(ValueKind::UInt);
  const ItemDescriptor int_d = numeric_descriptor(ValueKind::Int, true);
  const ItemDescriptor linear_d = numeric_descriptor(ValueKind::LinearLDS);
  const ItemDescriptor imapb_d = numeric_descriptor(ValueKind::IMAPB);
  const ItemDescriptor utf8_d = numeric_descriptor(ValueKind::Utf8);
  const ItemDescriptor bytes_d = numeric_descriptor(ValueKind::Bytes);
  const ItemDescriptor nested_d = numeric_descriptor(ValueKind::NestedLS);
  const ItemDescriptor pack_d = numeric_descriptor(ValueKind::Pack);

  // Every descriptor kind rejects a mismatched variant as an ordinary Result
  // error, rather than letting std::get throw.
  check_type_mismatch_no_throw(
      [&] { return codec::encode(uint_d, Value{std::int64_t{1}}, 1); },
      "UInt wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw(
      [&] { return codec::encode(int_d, Value{std::uint64_t{1}}, 1); },
      "Int wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw(
      [&] { return codec::encode(linear_d, Value{std::uint64_t{1}}, 1); },
      "LinearLDS wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw(
      [&] { return codec::encode(imapb_d, Value{std::uint64_t{1}}, 1); },
      "IMAPB wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw(
      [&] { return codec::encode(utf8_d, Value{std::uint64_t{1}}, 0); },
      "Utf8 wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw(
      [&] { return codec::encode(bytes_d, Value{std::uint64_t{1}}, 0); },
      "Bytes wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw(
      [&] { return codec::encode(nested_d, Value{std::uint64_t{1}}, 0); },
      "NestedLS wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw(
      [&] { return codec::encode(pack_d, Value{std::uint64_t{1}}, 0); },
      "Pack wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw(
      [&] {
        LocalSetBuilder b(gen::uas_0601);
        return b.set(2, Value{1.0});  // Precision Time Stamp is UInt
      },
      "LocalSetBuilder wrong Value alternative -> TypeMismatch (no throw)");

  // All numeric encoders reject the invalid widths before any shifting or
  // mapping arithmetic can occur.
  check_bad_numeric_lengths(uint_d, Value{std::uint64_t{0}},
                            "UInt widths 0 and 9 -> BadLength");
  check_bad_numeric_lengths(int_d, Value{std::int64_t{0}},
                            "Int widths 0 and 9 -> BadLength");
  check_bad_numeric_lengths(linear_d, Value{0.0},
                            "LinearLDS widths 0 and 9 -> BadLength");
  check_bad_numeric_lengths(imapb_d, Value{0.0},
                            "IMAPB widths 0 and 9 -> BadLength");

  // Width one makes the precise signed/unsigned representable limits obvious.
  check(static_cast<bool>(codec::encode(uint_d, Value{std::uint64_t{0}}, 1)) &&
            codec::encode(uint_d, Value{std::uint64_t{255}}, 1),
        "UInt width-1 boundaries succeed");
  auto uint_over = codec::encode(uint_d, Value{std::uint64_t{256}}, 1);
  check(!uint_over && uint_over.error() == Error::RangeError,
        "UInt width-1 overflow -> RangeError");
  check(static_cast<bool>(codec::encode(int_d, Value{std::int64_t{-128}}, 1)) &&
            codec::encode(int_d, Value{std::int64_t{127}}, 1),
        "Int width-1 boundaries succeed");
  auto int_low = codec::encode(int_d, Value{std::int64_t{-129}}, 1);
  auto int_high = codec::encode(int_d, Value{std::int64_t{128}}, 1);
  check(!int_low && int_low.error() == Error::RangeError &&
            !int_high && int_high.error() == Error::RangeError,
        "Int width-1 overflow -> RangeError");

  // At eight bytes, both LinearLDS endpoints must avoid floating-point
  // conversion overflow. The most-negative signed pattern is reserved, so the
  // minimum real endpoint is represented by INT64_MIN + 1.
  const ItemDescriptor signed_linear =
      numeric_descriptor(ValueKind::LinearLDS, true, {-100.0, 100.0});
  const ItemDescriptor unsigned_linear =
      numeric_descriptor(ValueKind::LinearLDS, false, {0.0, 100.0});
  const auto signed_min = codec::encode(signed_linear, Value{-100.0}, 8);
  const auto signed_max = codec::encode(signed_linear, Value{100.0}, 8);
  const auto unsigned_min = codec::encode(unsigned_linear, Value{0.0}, 8);
  const auto unsigned_max = codec::encode(unsigned_linear, Value{100.0}, 8);
  check_encoded_bytes(signed_min,
                      {B(0x80), B(0x00), B(0x00), B(0x00), B(0x00), B(0x00),
                       B(0x00), B(0x01)},
                      "signed LinearLDS width-8 minimum avoids reserved INT64_MIN");
  check_encoded_bytes(signed_max,
                      {B(0x7F), B(0xFF), B(0xFF), B(0xFF), B(0xFF), B(0xFF),
                       B(0xFF), B(0xFF)},
                      "signed LinearLDS width-8 maximum is INT64_MAX");
  check_encoded_bytes(unsigned_min,
                      {B(0x00), B(0x00), B(0x00), B(0x00), B(0x00), B(0x00),
                       B(0x00), B(0x00)},
                      "unsigned LinearLDS width-8 minimum is all zeroes");
  check_encoded_bytes(unsigned_max,
                      {B(0xFF), B(0xFF), B(0xFF), B(0xFF), B(0xFF), B(0xFF),
                       B(0xFF), B(0xFF)},
                      "unsigned LinearLDS width-8 maximum is all ones");

  for (double x : {-0.1, 100.1, std::numeric_limits<double>::quiet_NaN(),
                   std::numeric_limits<double>::infinity(),
                   -std::numeric_limits<double>::infinity()}) {
    auto r = codec::encode(linear_d, Value{x}, 2);
    check(!r && r.error() == Error::RangeError,
          "LinearLDS out-of-range/nonfinite -> RangeError");
  }

  // IMAPB preserves its ST 1201 structural-special behavior for the same input
  // classes, rather than turning them into RangeError.
  for (double x : {-0.1, 100.1, std::numeric_limits<double>::quiet_NaN(),
                   std::numeric_limits<double>::infinity(),
                   -std::numeric_limits<double>::infinity()}) {
    auto r = codec::encode(imapb_d, Value{x}, 2);
    check(r && codec::is_imap_special(*r),
          "IMAPB out-of-range/nonfinite -> structural special");
  }
}

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
  test_typed_encode_validation();
  test_multibyte_tag();
  test_roc_trimmed(full);
  test_malformed();
  std::printf("%s\n", failures == 0 ? "HARDENING: all PASS" : "HARDENING: FAIL");
  return failures == 0 ? 0 : 1;
}
