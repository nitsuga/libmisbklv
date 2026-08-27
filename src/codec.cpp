// SPDX-License-Identifier: Apache-2.0
#include "misbklv/codec.hpp"

#include <cmath>
#include <limits>
#include <string_view>
#include <variant>

namespace misbklv::codec {
namespace {

void wr_uint(Bytes& out, std::uint64_t v, int len) {
  for (int i = len - 1; i >= 0; --i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}

bool valid_numeric_width(std::size_t len) {
  return len >= 1 && len <= 8;
}

bool fits_uint(std::uint64_t v, std::size_t len) {
  return len == 8 || v <= ((std::uint64_t{1} << (8 * len)) - 1);
}

bool fits_int(std::int64_t v, std::size_t len) {
  if (len == 8) return true;
  const int bits = 8 * static_cast<int>(len);
  const std::int64_t max = (std::int64_t{1} << (bits - 1)) - 1;
  const std::int64_t min = -max - 1;
  return v >= min && v <= max;
}

std::int64_t rd_int(std::span<const std::byte> b) {
  std::uint64_t u = rd_uint(b);
  const int bits = 8 * static_cast<int>(b.size());
  if (bits < 64 && ((u >> (bits - 1)) & 1)) u |= (~0ull << bits);
  return static_cast<std::int64_t>(u);
}

// --- linear LDS map (legacy 0601; symmetric-signed or unsigned-affine) ------
double linear_decode(const ItemDescriptor& d, std::span<const std::byte> raw) {
  const int len = static_cast<int>(raw.size());
  if (d.is_signed) {
    // Unsigned shift: for len==8 a signed `1ll << 63` overflows into the sign
    // bit (UB). len is validated to 1..8 by decode(), so 8*len-1 is 7..63.
    const double imax = static_cast<double>((1ull << (8 * len - 1)) - 1);
    return static_cast<double>(rd_int(raw)) * d.map.max / imax;  // min == -max
  }
  const double denom =
      (len >= 8) ? 18446744073709551615.0 : static_cast<double>((1ull << (8 * len)) - 1);
  return d.map.min + static_cast<double>(rd_uint(raw)) * (d.map.max - d.map.min) / denom;
}
void linear_encode(const ItemDescriptor& d, double x, Bytes& out, int len) {
  if (d.is_signed) {
    const int bits = 8 * len;
    const std::uint64_t positive_max = (1ull << (bits - 1)) - 1;
    const std::uint64_t negative_max =
        static_cast<std::uint64_t>(-static_cast<std::int64_t>(positive_max));
    // LinearLDS reserves the most-negative integer. Encode the descriptor
    // endpoints explicitly: a double cannot distinguish INT64_MAX from 2^63.
    if (x == d.map.min) {
      wr_uint(out, negative_max, len);
      return;
    }
    if (x == d.map.max) {
      wr_uint(out, positive_max, len);
      return;
    }
    const double imax = static_cast<double>(positive_max);
    const double rounded = std::round(x * imax / d.map.max);
    // Clamp before conversion. For widths 7 and 8, `positive_max` may round
    // up in double precision; the bounds keep every float-to-int conversion
    // within the signed 64-bit range and avoid emitting the reserved code.
    const std::uint64_t encoded =
        !std::isfinite(rounded) || rounded <= -imax ? negative_max
        : rounded >= imax                           ? positive_max
                          : static_cast<std::uint64_t>(static_cast<std::int64_t>(rounded));
    wr_uint(out, encoded, len);
  } else {
    const double denom =
        (len >= 8) ? 18446744073709551615.0 : static_cast<double>((1ull << (8 * len)) - 1);
    const double rounded = std::round((x - d.map.min) * denom / (d.map.max - d.map.min));
    // As above, the all-ones 64-bit value rounds to 2^64 as a double.
    const std::uint64_t encoded = !std::isfinite(rounded) || rounded <= 0.0 ? 0
                                  : rounded >= 0x1p64 ? std::numeric_limits<std::uint64_t>::max()
                                                      : static_cast<std::uint64_t>(rounded);
    wr_uint(out, encoded, len);
  }
}

// --- ST 1201 IMAPB (Starting Point B): float [a,b] <-> unsigned L-byte int ---
struct ImapB {
  double sF, sR, zoff, a;
};
ImapB imapb_params(double a, double b, int len) {
  const double bPow = std::ceil(std::log2(b - a));  // ST 1201 §8, eq. bPow
  const double dPow = 8.0 * len - 1.0;
  ImapB p;
  p.sF = std::exp2(dPow - bPow);  // forward scaling factor
  p.sR = std::exp2(bPow - dPow);  // reverse scaling factor
  p.zoff = (a < 0.0 && b > 0.0) ? (p.sF * a - std::floor(p.sF * a)) : 0.0;
  p.a = a;
  return p;
}

// IMAP special values (ST 1201 §7.2.3, Tables 2-3): special iff top two bits set;
// identifier in the high bits of byte 0, remaining bytes zero-filled.
enum class ImapSpecial { None, PosInf, NegInf, NaNv, BelowMin, AboveMax, Reserved };

ImapSpecial imapb_special_of(std::uint8_t b0) {
  if ((b0 & 0xC0) != 0xC0) return ImapSpecial::None;  // top 2 bits not both set
  switch (b0 >> 3) {                                  // top 5 bits (Table 2)
    case 0x19:
      return ImapSpecial::PosInf;  // 11001
    case 0x1D:
      return ImapSpecial::NegInf;  // 11101
    case 0x1A:
    case 0x1E:
    case 0x1B:
    case 0x1F:  // +-QNaN / +-SNaN
      return ImapSpecial::NaNv;
    case 0x1C:  // 11100 -> MISB (Table 3)
      return (b0 & 0x01) ? ImapSpecial::AboveMax : ImapSpecial::BelowMin;
    default:
      return ImapSpecial::Reserved;
  }
}

void imapb_put_special(Bytes& out, std::uint8_t id, int len) {
  out.push_back(static_cast<std::byte>(id));
  for (int i = 1; i < len; ++i) out.push_back(std::byte{0});
}

}  // namespace

std::uint64_t rd_uint(std::span<const std::byte> b) {
  // Contract: 1..8 bytes (decode() enforces this before the internal callers
  // reach here). A direct caller inspecting raw bytes could pass more; a >8-byte
  // big-endian value cannot fit a uint64, so read only the least-significant 8
  // bytes — a defined result rather than an incidental high-byte shift-out.
  if (b.size() > 8) b = b.last(8);
  std::uint64_t v = 0;
  for (auto by : b) v = (v << 8) | std::to_integer<std::uint8_t>(by);
  return v;
}

double imapb_decode(const ItemDescriptor& d, std::span<const std::byte> raw) {
  // Same 1..8-byte contract as rd_uint: an over-long field cannot fit the
  // mapping's integer, so interpret only its low 8 bytes rather than letting
  // imapb_params see a bogus width. (decode() already rejects >8 up front.)
  if (raw.size() > 8) raw = raw.last(8);
  // A degenerate descriptor (min >= max) has no valid IMAPB scale: b - a <= 0
  // makes log2(b - a) non-finite in imapb_params. Surface it as NaN instead of a
  // meaningless value — the encode side emits the +QNaN special for the same case.
  if (!(d.map.min < d.map.max)) return std::numeric_limits<double>::quiet_NaN();
  if (!raw.empty()) {
    switch (imapb_special_of(std::to_integer<std::uint8_t>(raw[0]))) {
      case ImapSpecial::PosInf:
        return std::numeric_limits<double>::infinity();
      case ImapSpecial::NegInf:
        return -std::numeric_limits<double>::infinity();
      case ImapSpecial::NaNv:
      case ImapSpecial::Reserved:  // forward-compat: surface as NaN, don't crash
        return std::numeric_limits<double>::quiet_NaN();
      case ImapSpecial::BelowMin:
        return d.map.min;  // ST 1201 default reverse
      case ImapSpecial::AboveMax:
        return d.map.max;
      case ImapSpecial::None:
        break;
    }
  }
  const ImapB p = imapb_params(d.map.min, d.map.max, static_cast<int>(raw.size()));
  return p.sR * (static_cast<double>(rd_uint(raw)) - p.zoff) + p.a;
}

void imapb_encode(const ItemDescriptor& d, double x, Bytes& out, int len) {
  if (std::isnan(x)) return imapb_put_special(out, 0xD0, len);  // +QNaN
  if (std::isinf(x)) return imapb_put_special(out, x > 0 ? 0xC8 : 0xE8, len);
  if (x < d.map.min) return imapb_put_special(out, 0xE0, len);  // BELOW_MIN
  if (x > d.map.max) return imapb_put_special(out, 0xE1, len);  // ABOVE_MAX
  // A degenerate descriptor (min == max) reaches here only for x == min, since
  // any other x is caught by the BELOW_MIN/ABOVE_MAX guards above. With b - a == 0
  // imapb_params yields sF == +inf, so yf == inf*0 == NaN and the float->int cast
  // below would be UB. min > max is already unreachable (every finite x hits a
  // guard); reject any non-representable descriptor here for defensive parity
  // with linear_encode, emitting the +QNaN special (a defined, decodable output).
  if (!(d.map.min < d.map.max)) return imapb_put_special(out, 0xD0, len);
  const ImapB p = imapb_params(d.map.min, d.map.max, len);
  double yf = p.sF * (x - p.a) + p.zoff;  // truncate; x>=a => floor
  // A decoded on-grid value re-encodes to an exact integer in real arithmetic;
  // float error can pull it just below, so floor would drop 1 LSB. Nudge up by
  // a few ULP (far below a real cell width) to keep IMAPB a stable inverse.
  yf += (std::fabs(yf) + 1.0) * 8.0 * std::numeric_limits<double>::epsilon();
  // The descriptor guard above keeps yf finite; clamp the cast anyway so no
  // caller-supplied descriptor can drive the float->int conversion out of range.
  const double f = std::floor(yf);
  wr_uint(out, std::isfinite(f) && f > 0.0 ? static_cast<std::uint64_t>(f) : 0, len);
}

bool is_imap_special(std::span<const std::byte> raw) {
  return !raw.empty() && (std::to_integer<std::uint8_t>(raw[0]) & 0xC0) == 0xC0;
}

Result<Value> decode(const ItemDescriptor& d, std::span<const std::byte> raw) {
  // Numeric kinds need 1..8 bytes. Reject a crafted 0-length item (would drive
  // shift-by-negative UB in the int/linear/IMAP math) or an over-8-byte one
  // (rd_uint would silently wrap) before touching the value.
  switch (d.kind) {
    case ValueKind::UInt:
    case ValueKind::Int:
    case ValueKind::LinearLDS:
    case ValueKind::IMAPB:
      if (raw.empty() || raw.size() > 8) return Result<Value>::err(Error::BadLength);
      break;
    default:
      break;
  }
  switch (d.kind) {
    case ValueKind::UInt:
      return Result<Value>::ok(Value{rd_uint(raw)});
    case ValueKind::Int:
      return Result<Value>::ok(Value{rd_int(raw)});
    case ValueKind::LinearLDS:
      return Result<Value>::ok(Value{linear_decode(d, raw)});
    case ValueKind::IMAPB:
      return Result<Value>::ok(Value{imapb_decode(d, raw)});
    case ValueKind::Utf8:
      return Result<Value>::ok(
          Value{std::string_view{reinterpret_cast<const char*>(raw.data()), raw.size()}});
    default:
      return Result<Value>::ok(Value{raw});
  }
}

Result<Bytes> encode(const ItemDescriptor& d, const Value& v, std::size_t len) {
  Bytes out;
  switch (d.kind) {
    case ValueKind::UInt: {
      if (!std::holds_alternative<std::uint64_t>(v)) return Result<Bytes>::err(Error::TypeMismatch);
      if (!valid_numeric_width(len)) return Result<Bytes>::err(Error::BadLength);
      const auto value = std::get<std::uint64_t>(v);
      if (!fits_uint(value, len)) return Result<Bytes>::err(Error::RangeError);
      wr_uint(out, value, static_cast<int>(len));
      return Result<Bytes>::ok(std::move(out));
    }
    case ValueKind::Int: {
      if (!std::holds_alternative<std::int64_t>(v)) return Result<Bytes>::err(Error::TypeMismatch);
      if (!valid_numeric_width(len)) return Result<Bytes>::err(Error::BadLength);
      const auto value = std::get<std::int64_t>(v);
      if (!fits_int(value, len)) return Result<Bytes>::err(Error::RangeError);
      wr_uint(out, static_cast<std::uint64_t>(value), static_cast<int>(len));
      return Result<Bytes>::ok(std::move(out));
    }
    case ValueKind::LinearLDS: {
      if (!std::holds_alternative<double>(v)) return Result<Bytes>::err(Error::TypeMismatch);
      if (!valid_numeric_width(len)) return Result<Bytes>::err(Error::BadLength);
      const double value = std::get<double>(v);
      if (!std::isfinite(value) || value < d.map.min || value > d.map.max)
        return Result<Bytes>::err(Error::RangeError);
      linear_encode(d, value, out, static_cast<int>(len));
      return Result<Bytes>::ok(std::move(out));
    }
    case ValueKind::IMAPB: {
      if (!std::holds_alternative<double>(v)) return Result<Bytes>::err(Error::TypeMismatch);
      if (!valid_numeric_width(len)) return Result<Bytes>::err(Error::BadLength);
      // ST 1201 represents NaN, infinity, and values outside [min,max] as
      // structural special values rather than returning RangeError.
      imapb_encode(d, std::get<double>(v), out, static_cast<int>(len));
      return Result<Bytes>::ok(std::move(out));
    }
    case ValueKind::Utf8: {
      if (!std::holds_alternative<std::string_view>(v))
        return Result<Bytes>::err(Error::TypeMismatch);
      auto sv = std::get<std::string_view>(v);
      for (char c : sv) out.push_back(static_cast<std::byte>(c));
      return Result<Bytes>::ok(std::move(out));
    }
    case ValueKind::Bytes:
    case ValueKind::NestedLS:
    case ValueKind::Pack: {
      if (!std::holds_alternative<std::span<const std::byte>>(v))
        return Result<Bytes>::err(Error::TypeMismatch);
      auto sp = std::get<std::span<const std::byte>>(v);
      out.assign(sp.begin(), sp.end());
      return Result<Bytes>::ok(std::move(out));
    }
  }
  return Result<Bytes>::err(Error::TypeMismatch);
}

std::uint16_t bcc16(std::span<const std::byte> data) {
  std::uint16_t sum = 0;
  for (std::size_t i = 0; i < data.size(); ++i)
    sum += static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data[i]))
           << (8 * ((i + 1) % 2));
  return sum;
}

}  // namespace misbklv::codec
