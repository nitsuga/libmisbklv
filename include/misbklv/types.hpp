// SPDX-License-Identifier: Apache-2.0
// Core KLV data-model types (ADR 0005) + registry descriptor schema (ADR 0010).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

namespace misbklv {

// --- Error / Result (ADR 0007) ---------------------------------------------
enum class Error : std::uint8_t {
  // decode
  Truncated,
  BadLength,
  UnknownTag,
  OutOfRange,
  // encode (ADR 0011 extends the enum)
  MissingMandatory,
  RangeError,
  // media backend (ADR 0013)
  Backend,      // pipeline / I/O failure
  Unsupported,  // not implemented in this backend / config
  // Typed encode validation; appended to preserve the existing numeric codes.
  TypeMismatch,  // Value variant does not match the ItemDescriptor::kind
  ResourceLimit,  // configured parser/backend resource limit exceeded
};

// Minimal Result<T> — the full map/and_then API is deferred (ADR 0007).
template <class T>
class Result {
 public:
  static Result ok(T v) { return Result(std::move(v)); }
  static Result err(Error e) { return Result(e); }

  explicit operator bool() const { return ok_; }
  T& operator*() { return value_; }
  const T& operator*() const { return value_; }
  T* operator->() { return &value_; }
  Error error() const { return error_; }

 private:
  explicit Result(T v) : ok_(true), value_(std::move(v)) {}
  explicit Result(Error e) : ok_(false), error_(e), value_() {}
  bool ok_;
  Error error_{};
  T value_;
};

// --- Descriptor schema (ADR 0010) ------------------------------------------
enum class ValueKind : std::uint8_t {
  UInt,       // raw big-endian unsigned integer
  Int,        // raw big-endian signed integer
  LinearLDS,  // legacy 0601 linear int->float map (map.min/max)
  IMAPB,      // ST 1201 mapping (not exercised in M1)
  Utf8,
  Bytes,
  NestedLS,
  Pack,
};

enum ItemFlags : std::uint8_t { kNone = 0, kMandatory = 1u << 0 };

// Closed set of registries (ADR 0010). A NestedLS/Pack item routes into a child
// via its `child` id; `registry_for()` (see registries.hpp) resolves it.
enum class RegistryId : std::uint8_t { None, Uas0601, Vmti0903, Vtarget0903 };

struct SpecialValue {
  std::uint64_t pattern;  // raw KLV bits (width = item length)
};

struct MappingParams {
  double min;
  double max;
};

struct ItemDescriptor {
  std::uint16_t tag;
  std::string_view name;
  std::string_view units;
  ValueKind kind;
  bool variable;            // length is variable in the standard (else fixed_len bytes)
  std::uint8_t fixed_len;   // encode width: the default when variable, else the fixed width
  bool is_signed;           // LinearLDS / Int
  MappingParams map;        // LinearLDS / IMAPB only
  std::span<const SpecialValue> specials;
  std::uint8_t flags;
  RegistryId child;         // NestedLS / Pack: the sub-registry to descend into
};

struct Registry {
  std::string_view id;
  std::span<const ItemDescriptor> items;    // sorted by tag
  std::span<const std::uint8_t> ul_key;     // 16-byte UL for a standalone packet

  const ItemDescriptor* find(std::uint16_t tag) const {
    const ItemDescriptor* lo = items.data();
    const ItemDescriptor* hi = lo + items.size();
    while (lo < hi) {
      const ItemDescriptor* mid = lo + (hi - lo) / 2;
      if (mid->tag < tag) {
        lo = mid + 1;
      } else if (mid->tag > tag) {
        hi = mid;
      } else {
        return mid;
      }
    }
    return nullptr;
  }
};

// --- Typed view variant (ADR 0005; tag set = ValueKind, ADR 0010) ----------
using Value = std::variant<std::uint64_t,             // UInt
                           std::int64_t,              // Int
                           double,                    // LinearLDS / IMAPB
                           std::string_view,          // Utf8
                           std::span<const std::byte>  // Bytes / opaque
                           >;

}  // namespace misbklv
