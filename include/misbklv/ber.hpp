// SPDX-License-Identifier: Apache-2.0
// BER-OID tag + BER length codec (ADR 0005 layer 1; ST 0107 §6.3).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "misbklv/types.hpp"

namespace misbklv::ber {

using Bytes = std::vector<std::byte>;

struct Parsed {
  std::uint64_t value;
  std::size_t consumed;
};

// BER-OID tag: base-128, big-endian, high bit = continuation (ST 0107 §6.3.1).
Result<Parsed> read_oid(std::span<const std::byte> buf, std::size_t pos);
void write_oid(Bytes& out, std::uint64_t tag);

// BER length: short form (<0x80) or long form (0x8N + N count bytes). ST 0107 §6.3.2.
Result<Parsed> read_length(std::span<const std::byte> buf, std::size_t pos);
void write_length(Bytes& out, std::uint64_t len);  // fewest bytes (ST 0107 3-05)

}  // namespace misbklv::ber
