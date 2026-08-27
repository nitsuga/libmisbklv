// SPDX-License-Identifier: Apache-2.0
// Real-world hardening: inputs the clean vendor samples never exercised.
//  (1) multi-byte BER-OID tags (>=128, e.g. 0601 Item 143 MSID),
//  (2) Report-on-Change trimmed packets (fewer items than a full frame),
//  (3) malformed / adversarial bytes must return a Result error, never crash
//      or invoke UB (run under -fsanitize=address,undefined via MISBKLV_SANITIZE),
//  (4) BER parser boundaries retain valid maxima and reject overflow/aliasing.
// argv: <input.klv>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <limits>
#include <span>
#include <vector>

#include "misbklv/builder.hpp"
#include "misbklv/ber.hpp"
#include "misbklv/codec.hpp"
#include "misbklv/packet.hpp"
#include "misbklv/series.hpp"
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
  std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i)
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  return out;
}
static std::byte B(int v) {
  return static_cast<std::byte>(v & 0xFF);
}

static ItemDescriptor numeric_descriptor(ValueKind kind, bool is_signed = false,
                                         MappingParams map = {0.0, 100.0}) {
  ItemDescriptor d{};
  d.kind = kind;
  d.is_signed = is_signed;
  d.map = map;
  return d;
}

template <class F> static void check_type_mismatch_no_throw(F&& f, const char* what) {
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

static void check_bad_numeric_lengths(const ItemDescriptor& d, const Value& v, const char* what) {
  auto zero = codec::encode(d, v, 0);
  auto nine = codec::encode(d, v, 9);
  check(!zero && zero.error() == Error::BadLength, what);
  check(!nine && nine.error() == Error::BadLength, what);
}

static void check_encoded_bytes(const Result<ber::Bytes>& got,
                                std::initializer_list<std::byte> want, const char* what) {
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
  check_type_mismatch_no_throw([&] { return codec::encode(uint_d, Value{std::int64_t{1}}, 1); },
                               "UInt wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw([&] { return codec::encode(int_d, Value{std::uint64_t{1}}, 1); },
                               "Int wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw([&] { return codec::encode(linear_d, Value{std::uint64_t{1}}, 1); },
                               "LinearLDS wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw([&] { return codec::encode(imapb_d, Value{std::uint64_t{1}}, 1); },
                               "IMAPB wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw([&] { return codec::encode(utf8_d, Value{std::uint64_t{1}}, 0); },
                               "Utf8 wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw([&] { return codec::encode(bytes_d, Value{std::uint64_t{1}}, 0); },
                               "Bytes wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw([&] { return codec::encode(nested_d, Value{std::uint64_t{1}}, 0); },
                               "NestedLS wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw([&] { return codec::encode(pack_d, Value{std::uint64_t{1}}, 0); },
                               "Pack wrong Value alternative -> TypeMismatch (no throw)");
  check_type_mismatch_no_throw(
      [&] {
        LocalSetBuilder b(gen::uas_0601);
        return b.set(2, Value{1.0});  // Precision Time Stamp is UInt
      },
      "LocalSetBuilder wrong Value alternative -> TypeMismatch (no throw)");

  // All numeric encoders reject the invalid widths before any shifting or
  // mapping arithmetic can occur.
  check_bad_numeric_lengths(uint_d, Value{std::uint64_t{0}}, "UInt widths 0 and 9 -> BadLength");
  check_bad_numeric_lengths(int_d, Value{std::int64_t{0}}, "Int widths 0 and 9 -> BadLength");
  check_bad_numeric_lengths(linear_d, Value{0.0}, "LinearLDS widths 0 and 9 -> BadLength");
  check_bad_numeric_lengths(imapb_d, Value{0.0}, "IMAPB widths 0 and 9 -> BadLength");

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
  check(!int_low && int_low.error() == Error::RangeError && !int_high &&
            int_high.error() == Error::RangeError,
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
                      {B(0x80), B(0x00), B(0x00), B(0x00), B(0x00), B(0x00), B(0x00), B(0x01)},
                      "signed LinearLDS width-8 minimum avoids reserved INT64_MIN");
  check_encoded_bytes(signed_max,
                      {B(0x7F), B(0xFF), B(0xFF), B(0xFF), B(0xFF), B(0xFF), B(0xFF), B(0xFF)},
                      "signed LinearLDS width-8 maximum is INT64_MAX");
  check_encoded_bytes(unsigned_min,
                      {B(0x00), B(0x00), B(0x00), B(0x00), B(0x00), B(0x00), B(0x00), B(0x00)},
                      "unsigned LinearLDS width-8 minimum is all zeroes");
  check_encoded_bytes(unsigned_max,
                      {B(0xFF), B(0xFF), B(0xFF), B(0xFF), B(0xFF), B(0xFF), B(0xFF), B(0xFF)},
                      "unsigned LinearLDS width-8 maximum is all ones");

  for (double x :
       {-0.1, 100.1, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
    auto r = codec::encode(linear_d, Value{x}, 2);
    check(!r && r.error() == Error::RangeError, "LinearLDS out-of-range/nonfinite -> RangeError");
  }

  // IMAPB preserves its ST 1201 structural-special behavior for the same input
  // classes, rather than turning them into RangeError.
  for (double x :
       {-0.1, 100.1, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
    auto r = codec::encode(imapb_d, Value{x}, 2);
    check(r && codec::is_imap_special(*r), "IMAPB out-of-range/nonfinite -> structural special");
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
    if (it.tag == 143 && it.value.size() == 3 && it.value[0] == B(0xAB)) ok143 = true;
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
    if (it.tag == 1) continue;  // checksum re-emitted
    if (it.tag != 2 && it.tag != 5 && it.tag != 6) continue;
    b.append_raw(it.tag, it.value);  // raw preserves exact bytes
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
  huge.push_back(B(0x88));  // long form, 8 length bytes
  for (int i = 0; i < 8; ++i) huge.push_back(B(0xFF));
  check(!parse_packet(huge), "8-byte huge length -> Truncated (no OOB)");
  check(packet_frame_length(huge) == 0, "frame_length rejects huge length");

  // A complete-looking frame is not KLV unless it begins with the SMPTE UL
  // prefix. This is also the gate the GStreamer extractor uses to resync.
  std::vector<std::byte> bogus_key(17, B(0xA5));
  bogus_key[16] = B(0x00);  // a complete, zero-length short-form BER value
  check(packet_frame_length(bogus_key) == 0,
        "frame_length rejects non-UL key with complete BER length");

  // Bare TLV with a huge item length.
  std::vector<std::byte> item{B(0x05), B(0x88)};
  for (int i = 0; i < 8; ++i) item.push_back(B(0xFF));
  check(!parse_items(item), "item with huge length -> Truncated (no OOB)");

  // Zero-length + over-long numeric decode (shift-UB / rd_uint wrap guards).
  const ItemDescriptor* sgn = gen::uas_0601.find(13);  // SensorLatitude, signed
  const ItemDescriptor* u = gen::uas_0601.find(2);     // Precision Time, UInt
  check(sgn && !codec::decode(*sgn, {}), "0-length signed decode -> BadLength");
  std::vector<std::byte> nine(9, B(0));
  check(u && !codec::decode(*u, nine), "9-byte UInt decode -> BadLength");

  // Garbage fed to the TS extractor must not crash.
  std::vector<std::byte> garbage(1000, B(0xA5));
  bool extract_returned = true;  // reaching here at all means no crash/UB
  extract_ts_klv(garbage, [](const KlvPacket&) {});
  check(extract_returned, "extract_ts_klv(garbage) returns without crashing");
}

// --- (4) BER parser boundary values ----------------------------------------
static void test_parser_boundaries() {
  std::printf("(4) BER parser boundary values\n");

  // A length of UINT64_MAX must not wrap the series bounds check.
  std::vector<std::byte> huge_series{B(0x88)};
  for (int i = 0; i < 8; ++i) huge_series.push_back(B(0xFF));
  auto series = parse_vtarget_series(huge_series);
  check(!series && series.error() == Error::Truncated,
        "vTarget series UINT64_MAX length -> Truncated");

  const std::vector<std::byte> leading_zero_bytes{B(0x80)};
  auto leading_zero = ber::read_oid(leading_zero_bytes, 0);
  check(!leading_zero && leading_zero.error() == Error::BadLength,
        "BER-OID leading 0x80 -> BadLength");

  const std::vector<std::byte> truncated_chain_bytes{B(0x81)};
  auto truncated_chain = ber::read_oid(truncated_chain_bytes, 0);
  check(!truncated_chain && truncated_chain.error() == Error::Truncated,
        "truncated BER-OID continuation chain -> Truncated");

  // UINT64_MAX occupies ten base-128 groups: 1 followed by nine 0x7f groups.
  std::vector<std::byte> max_oid{B(0x81)};
  for (int i = 0; i < 8; ++i) max_oid.push_back(B(0xFF));
  max_oid.push_back(B(0x7F));
  auto decoded_max = ber::read_oid(max_oid, 0);
  check(decoded_max && decoded_max->value == std::numeric_limits<std::uint64_t>::max() &&
            decoded_max->consumed == max_oid.size(),
        "canonical UINT64_MAX BER-OID decodes");

  // The next base-128 value is 2^64 and cannot fit in the API's uint64_t.
  std::vector<std::byte> beyond_max{B(0x82)};
  for (int i = 0; i < 8; ++i) beyond_max.push_back(B(0x80));
  beyond_max.push_back(B(0x00));
  auto decoded_beyond = ber::read_oid(beyond_max, 0);
  check(!decoded_beyond && decoded_beyond.error() == Error::OutOfRange,
        "BER-OID 2^64 -> OutOfRange");

  // 65535 is the largest Item::tag and must retain its full identity.
  const std::vector<std::byte> max_tag{B(0x83), B(0xFF), B(0x7F), B(0x00)};
  auto parsed_max_tag = parse_items(max_tag);
  check(parsed_max_tag && parsed_max_tag->size() == 1 && parsed_max_tag->front().tag == 65535,
        "parse_items accepts tag 65535");

  // 65537 must fail, rather than narrowing to uint16_t tag 1 (the checksum).
  const std::vector<std::byte> alias_checksum{B(0x84), B(0x80), B(0x01), B(0x00)};
  auto parsed_alias = parse_items(alias_checksum);
  check(!parsed_alias && parsed_alias.error() == Error::OutOfRange,
        "parse_items tag 65537 -> OutOfRange (cannot alias checksum tag 1)");

  // Public rd_uint has no error channel (issue #7 item 2): a direct caller
  // passing > 8 bytes must get a defined value — the low 8 bytes, not an
  // incidental shift-out wrap. Nine bytes 01 00..00 08: the leading 0x01 is
  // dropped, leaving 0x08.
  std::vector<std::byte> nine{B(0x01)};
  for (int i = 0; i < 7; ++i) nine.push_back(B(0x00));
  nine.push_back(B(0x08));
  std::vector<std::byte> low8(nine.begin() + 1, nine.end());
  check(codec::rd_uint(nine) == 0x08 && codec::rd_uint(nine) == codec::rd_uint(low8),
        "rd_uint(>8 bytes) reads the low 8 bytes, not a wrap");

  // Non-minimal BER long-form length is accepted, by decision (ADR 0030): a
  // robust reader over a strict writer. 0x81 0x05 and 0x82 0x00 0x05 both mean 5.
  const std::vector<std::byte> nonmin1{B(0x81), B(0x05)};
  auto len1 = ber::read_length(nonmin1, 0);
  check(len1 && len1->value == 5 && len1->consumed == 2,
        "non-minimal BER length 0x81 0x05 -> 5 (accepted)");
  const std::vector<std::byte> nonmin2{B(0x82), B(0x00), B(0x05)};
  auto len2 = ber::read_length(nonmin2, 0);
  check(len2 && len2->value == 5 && len2->consumed == 3,
        "non-minimal BER length 0x82 0x00 0x05 -> 5 (accepted)");
}

// --- (5) bounded live-frame inspection -------------------------------------
static void test_bounded_frame_inspection() {
  std::printf("(5) bounded live-frame inspection\n");
  constexpr std::size_t cap = 20;
  std::vector<std::byte> ul;
  for (std::uint8_t b : kUas0601Key) ul.push_back(B(b));

  auto empty = inspect_packet_frame({}, cap);
  check(empty && !*empty, "empty frame inspection -> incomplete");
  auto partial_ul = inspect_packet_frame(std::span<const std::byte>(ul).first(3), cap);
  check(partial_ul && !*partial_ul, "partial matching UL -> incomplete");

  const std::vector<std::byte> invalid_prefix{B(0x06), B(0x0E), B(0x2B), B(0x35)};
  auto invalid = inspect_packet_frame(invalid_prefix, cap);
  check(!invalid && invalid.error() == Error::BadLength, "nonmatching UL prefix -> BadLength");

  auto no_length = inspect_packet_frame(ul, cap);
  check(no_length && !*no_length, "valid UL with missing BER length -> incomplete");
  auto split_length = ul;
  split_length.push_back(B(0x82));
  split_length.push_back(B(0x00));
  auto partial_length = inspect_packet_frame(split_length, cap);
  check(partial_length && !*partial_length, "valid UL with split legal BER length -> incomplete");

  auto indefinite = ul;
  indefinite.push_back(B(0x80));
  auto bad_indefinite = inspect_packet_frame(indefinite, cap);
  check(!bad_indefinite && bad_indefinite.error() == Error::BadLength,
        "indefinite BER length -> BadLength");
  auto excessive_count = ul;
  excessive_count.push_back(B(0x89));
  auto bad_count = inspect_packet_frame(excessive_count, cap);
  check(!bad_count && bad_count.error() == Error::BadLength,
        "BER length count over eight -> BadLength");

  // 16-byte UL + one-byte length + four-byte value is over a 20-byte cap,
  // even though none of that value has arrived yet.
  auto over_cap = ul;
  over_cap.push_back(B(0x04));
  auto refused = inspect_packet_frame(over_cap, cap);
  check(!refused && refused.error() == Error::ResourceLimit,
        "declared frame over cap -> ResourceLimit before payload");

  auto exact_cap = ul;
  exact_cap.insert(exact_cap.end(), {B(0x03), B(0x00), B(0x00), B(0x00)});
  auto complete = inspect_packet_frame(exact_cap, cap);
  check(complete && *complete && **complete == cap, "complete frame exactly at cap succeeds");
  auto underfilled = exact_cap;
  underfilled.pop_back();
  auto incomplete = inspect_packet_frame(underfilled, cap);
  check(incomplete && !*incomplete, "within-cap declared frame missing bytes -> incomplete");

  // The compatibility framer stays unbounded and returns only a complete size.
  check(packet_frame_length(exact_cap) == cap, "packet_frame_length compatibility complete frame");
  check(packet_frame_length(underfilled) == 0,
        "packet_frame_length compatibility incomplete frame");
}

// --- (6) extract_ts_klv malformed-input robustness (issue #3) ---------------
// The gst-free extractor must match the live extractor's drain() robustness:
// fail terminally on corrupt framing instead of stalling, resync over garbage,
// and reassemble KLV packets that span several PES packets. The extractor is
// content-based (the KLV PID is the first PID whose PES payload starts with
// the SMPTE UL), so these carriers need no PAT/PMT.
namespace {

// One 188-byte TS packet carrying `payload` on `pid`. `pusi` sets the
// payload_unit_start_indicator; `cc` is the continuity counter (low 4 bits of
// the header's 4th byte). A payload of exactly 184 bytes fills the packet;
// anything shorter is padded with an adaptation field.
std::array<std::byte, 188> ts_packet(std::uint16_t pid, bool pusi, std::uint8_t cc,
                                     std::span<const std::byte> payload) {
  std::array<std::byte, 188> pkt{};
  pkt[0] = B(0x47);
  pkt[1] = B((pusi ? 0x40 : 0) | static_cast<int>((pid >> 8) & 0x1F));
  pkt[2] = B(pid & 0xFF);
  if (payload.size() == 184) {  // exactly fills the packet, no stuffing room
    pkt[3] = B(0x10 | (cc & 0x0F));
    std::copy(payload.begin(), payload.end(), pkt.begin() + 4);
  } else {  // pad the remainder with an adaptation field
    pkt[3] = B(0x30 | (cc & 0x0F));
    const std::size_t adapt = 183 - payload.size();
    pkt[4] = B(adapt);
    if (adapt > 0) {
      pkt[5] = B(0x00);  // adaptation flags, no PCR
      std::fill(pkt.begin() + 6, pkt.begin() + 6 + adapt - 1, B(0xFF));
    }
    std::copy(payload.begin(), payload.end(), pkt.begin() + 5 + adapt);
  }
  return pkt;
}

// A complete PES: 00 00 01 <stream_id> <len_hi> <len_lo> + payload. stream_id
// 0xC0 (private_stream_2) has no optional PES header, so the payload starts
// right after the 6-byte header — see unwrap_pes() in src/ts.cpp.
std::vector<std::byte> pes_packet(std::uint8_t stream_id, std::span<const std::byte> payload) {
  std::vector<std::byte> pes;
  pes.push_back(B(0x00));
  pes.push_back(B(0x00));
  pes.push_back(B(0x01));
  pes.push_back(B(stream_id));
  pes.push_back(B(payload.size() >> 8));
  pes.push_back(B(payload.size() & 0xFF));
  pes.insert(pes.end(), payload.begin(), payload.end());
  return pes;
}

// 0x15 variant: the KLV rides inside an SMPTE RP 217 metadata AU cell, whose
// 5-byte header precedes the cell payload (service id 0, sequence number, and
// fragmentation flags 0xDF for one complete cell) — mirroring the fixture
// generator's construction. unwrap_pes() strips the header and trusts its
// 16-bit length, so each fragment gets a cell header of its own.
std::vector<std::byte> au_cell_pes(std::uint8_t seq, std::span<const std::byte> klv_fragment) {
  std::vector<std::byte> cell;
  cell.push_back(B(0x00));
  cell.push_back(B(seq));
  cell.push_back(B(0xDF));
  cell.push_back(B(klv_fragment.size() >> 8));
  cell.push_back(B(klv_fragment.size() & 0xFF));
  cell.insert(cell.end(), klv_fragment.begin(), klv_fragment.end());
  return pes_packet(0xFC, cell);
}

// Packetize one PES into 188-byte TS packets on `pid`, advancing `cc`.
std::vector<std::byte> packetize_pes(std::uint16_t pid, const std::vector<std::byte>& pes,
                                     std::uint8_t& cc) {
  std::vector<std::byte> out;
  std::size_t off = 0;
  bool first = true;
  while (off < pes.size()) {
    const std::size_t take = std::min<std::size_t>(184, pes.size() - off);
    auto pkt = ts_packet(pid, first, cc, std::span(pes).subspan(off, take));
    out.insert(out.end(), pkt.begin(), pkt.end());
    cc = static_cast<std::uint8_t>((cc + 1) & 0x0F);
    off += take;
    first = false;
  }
  return out;
}

// BER length octets (src/ber.cpp write_length semantics): short form below
// 0x80, long form with a count byte above it.
std::vector<std::byte> ber_length(std::size_t len) {
  std::vector<std::byte> out;
  if (len < 0x80) {
    out.push_back(B(len));
    return out;
  }
  std::byte tmp[8];
  int n = 0;
  while (len) {
    tmp[n++] = B(len & 0xFF);
    len >>= 8;
  }
  out.push_back(B(0x80 | n));
  while (n > 0) out.push_back(tmp[--n]);
  return out;
}

// A complete KLV packet: 16-byte UL key + BER length + TLV items.
std::vector<std::byte> klv_packet(const std::vector<std::byte>& items) {
  std::vector<std::byte> pkt;
  for (std::uint8_t b : kUas0601Key) pkt.push_back(B(b));
  auto len = ber_length(items.size());
  pkt.insert(pkt.end(), len.begin(), len.end());
  pkt.insert(pkt.end(), items.begin(), items.end());
  return pkt;
}

// Run the extractor, collecting delivered packets.
Result<std::monostate> run_extract(const std::vector<std::byte>& ts,
                                   std::vector<std::vector<std::byte>>& out) {
  return extract_ts_klv(
      ts, [&](const KlvPacket& kp) { out.emplace_back(kp.bytes.begin(), kp.bytes.end()); });
}

}  // namespace

static void test_ts_klv_robustness() {
  std::printf("(6) extract_ts_klv malformed-input robustness\n");
  constexpr std::uint16_t kPid = 0x0101;
  std::uint8_t cc = 0;
  auto append_pes = [&](std::vector<std::byte>& stream, const std::vector<std::byte>& pes) {
    auto packets = packetize_pes(kPid, pes, cc);
    stream.insert(stream.end(), packets.begin(), packets.end());
  };

  // Two distinguishable, well-formed local sets (tags 2 + 5 TLVs).
  const std::vector<std::byte> items_a = {
      B(0x02), B(0x04), B(0x00), B(0x00), B(0x00), B(0x2A),  // Precision Time Stamp
      B(0x05), B(0x01), B(0x7F),                             // Frame Center Elevation
  };
  const std::vector<std::byte> items_b = {
      B(0x02), B(0x04), B(0x00), B(0x00), B(0x00), B(0x3C), B(0x05), B(0x01), B(0x55),
  };
  const auto pkt_a = klv_packet(items_a);
  const auto pkt_b = klv_packet(items_b);

  // (1) Corrupt declared BER length after a valid UL: the old
  // packet_frame_length() framer returned 0 for both "incomplete" and
  // "malformed", stalling forever with no error; now the extract must fail
  // terminally with BadLength, keeping the valid packet already delivered.
  std::vector<std::byte> ts1;
  append_pes(ts1, pes_packet(0xC0, pkt_a));
  {
    std::vector<std::byte> corrupt;
    for (std::uint8_t b : kUas0601Key) corrupt.push_back(B(b));
    corrupt.push_back(B(0x89));  // long form, 9 length octets — illegal (max 8)
    append_pes(ts1, pes_packet(0xC0, corrupt));
  }
  std::vector<std::vector<std::byte>> out1;
  auto r1 = run_extract(ts1, out1);
  check(!r1 && r1.error() == Error::BadLength,
        "extract_ts_klv corrupt declared BER length -> BadLength");
  check(out1.size() == 1 && out1[0] == pkt_a, "packet before the corruption stays delivered");

  // (2) Declared frame over the 16 MiB reassembly cap: a tiny buffer with a
  // huge declared length must fail with ResourceLimit, not wait forever.
  std::vector<std::byte> over_cap;
  for (std::uint8_t b : kUas0601Key) over_cap.push_back(B(b));
  over_cap.push_back(B(0x83));  // long form, 3 length octets
  over_cap.push_back(B(0xFF));
  over_cap.push_back(B(0xFF));
  over_cap.push_back(B(0xFF));  // declared value length 16,777,215 > 16 MiB
  std::vector<std::byte> ts2;
  append_pes(ts2, pes_packet(0xC0, over_cap));
  std::vector<std::vector<std::byte>> out2;
  auto r2 = run_extract(ts2, out2);
  check(!r2 && r2.error() == Error::ResourceLimit,
        "extract_ts_klv declared frame over 16 MiB cap -> ResourceLimit");
  check(out2.empty(), "over-cap frame delivers nothing");

  // (3) Garbage injected after packet A (same PES) with packet B in the next
  // PES: resync must skip the garbage and deliver both packets byte-exact.
  std::vector<std::byte> payload3 = pkt_a;
  payload3.insert(payload3.end(), {B(0xDE), B(0xAD), B(0xBE), B(0xEF), B(0x00), B(0x01)});
  std::vector<std::byte> ts3;
  append_pes(ts3, pes_packet(0xC0, payload3));
  append_pes(ts3, pes_packet(0xC0, pkt_b));
  std::vector<std::vector<std::byte>> out3;
  auto r3 = run_extract(ts3, out3);
  check(static_cast<bool>(r3), "extract_ts_klv garbage between packets succeeds");
  check(out3.size() == 2 && out3[0] == pkt_a && out3[1] == pkt_b,
        "both packets extracted byte-exact, garbage skipped");

  // (4) One KLV packet split across two consecutive PES packets on the KLV
  // PID: the second PES payload does not start with the UL, but reassembly
  // joins the fragments and extracts the single packet byte-exact.
  const std::size_t split = 20;  // inside pkt_a, past the UL + length byte
  std::vector<std::byte> head(pkt_a.begin(), pkt_a.begin() + split);
  std::vector<std::byte> tail(pkt_a.begin() + split, pkt_a.end());
  std::vector<std::byte> ts4;
  append_pes(ts4, pes_packet(0xC0, head));
  append_pes(ts4, pes_packet(0xC0, tail));
  std::vector<std::vector<std::byte>> out4;
  auto r4 = run_extract(ts4, out4);
  check(static_cast<bool>(r4), "extract_ts_klv packet split across PES succeeds");
  check(out4.size() == 1 && out4[0] == pkt_a, "split KLV packet reassembled byte-exact");

  // 0x15 variant of (4): each fragment rides in its own RP 217 AU cell, so
  // each PES payload has a cell header and only the first starts with the UL.
  std::vector<std::byte> ts6;
  {
    const std::size_t split15 = 18;
    std::vector<std::byte> chead(pkt_a.begin(), pkt_a.begin() + split15);
    std::vector<std::byte> ctail(pkt_a.begin() + split15, pkt_a.end());
    append_pes(ts6, au_cell_pes(0, chead));
    append_pes(ts6, au_cell_pes(1, ctail));
  }
  std::vector<std::vector<std::byte>> out6;
  auto r6 = run_extract(ts6, out6);
  check(static_cast<bool>(r6), "extract_ts_klv 0x15 AU cell split across PES succeeds");
  check(out6.size() == 1 && out6[0] == pkt_a, "split 0x15 KLV packet reassembled byte-exact");
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: hardening_test <input.klv>\n");
    return 2;
  }
  const char* path = argv[1];
  const auto full = read_file(path);
  if (full.empty()) {
    std::fprintf(stderr, "could not read fixture: %s\n", path);
    return 2;
  }
  test_typed_encode_validation();
  test_multibyte_tag();
  test_roc_trimmed(full);
  test_malformed();
  test_parser_boundaries();
  test_bounded_frame_inspection();
  test_ts_klv_robustness();
  std::printf("%s\n", failures == 0 ? "HARDENING: all PASS" : "HARDENING: FAIL");
  return failures == 0 ? 0 : 1;
}
