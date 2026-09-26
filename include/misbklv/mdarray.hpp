// SPDX-License-Identifier: Apache-2.0
// MISB ST 1303 Multi-Dimensional Array Pack (MDAP) structural decoder.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "misbklv/types.hpp"

namespace misbklv::mdarray {

enum class Apa : std::uint8_t {
  Natural = 1,
  Imapb = 2,
  BooleanCompact = 3,
  UintCompact = 4,
  RunLength = 5,
};

struct MdArray {
  std::vector<std::uint64_t> dims;
  std::uint32_t ebytes{};
  Apa apa{};
  std::span<const std::byte> apas;
  std::span<const std::byte> elements;

  std::size_t element_count() const;
  bool has_data() const { return ebytes != 0; }
  Result<std::uint64_t> element_uint(std::size_t flat_index) const;
  Result<double> element_float(std::size_t flat_index) const;
  Result<double> element_imapb(std::size_t flat_index) const;
};

// Decode an MDAP value. Natural and Boolean element lengths are validated;
// Compact element payloads remain raw because their inner formats differ.
Result<MdArray> parse(std::span<const std::byte> value);

}  // namespace misbklv::mdarray
