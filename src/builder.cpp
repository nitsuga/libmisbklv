// SPDX-License-Identifier: Apache-2.0
#include "misbklv/builder.hpp"

#include "misbklv/codec.hpp"

namespace misbklv {

Result<std::monostate> LocalSetBuilder::set(std::uint16_t tag, const Value& v,
                                            std::optional<std::size_t> len) {
  const ItemDescriptor* d = reg_.find(tag);
  if (!d) return Result<std::monostate>::err(Error::UnknownTag);
  auto enc = codec::encode(*d, v, len.value_or(d->fixed_len));
  if (!enc) return Result<std::monostate>::err(enc.error());
  staged_.emplace_back(tag, std::move(*enc));
  return Result<std::monostate>::ok({});
}

void LocalSetBuilder::append_raw(std::uint16_t tag, std::span<const std::byte> bytes) {
  staged_.emplace_back(tag, ber::Bytes(bytes.begin(), bytes.end()));
}

// Callers exclude the checksum item before staging (finalize() emits a fresh
// one); tag 1 is NOT special here — in embedded LSs (e.g. VTarget) it is data.
ber::Bytes LocalSetBuilder::serialize_items() const {
  ber::Bytes out;
  for (const auto& [tag, val] : staged_) {
    ber::write_oid(out, tag);
    ber::write_length(out, val.size());
    out.insert(out.end(), val.begin(), val.end());
  }
  return out;
}

bool LocalSetBuilder::staged_contains(std::uint16_t tag) const {
  for (const auto& s : staged_)
    if (s.first == tag) return true;
  return false;
}

Result<ber::Bytes> LocalSetBuilder::finalize(std::span<const std::uint8_t> ul_key,
                                             bool enforce_mandatory) && {
  // 1. mandatory-item check (ADR 0011).
  if (enforce_mandatory)
    for (const auto& d : reg_.items)
      if ((d.flags & kMandatory) && !staged_contains(d.tag))
        return Result<ber::Bytes>::err(Error::MissingMandatory);

  // 2. serialize staged items (callers already excluded the checksum).
  ber::Bytes items = serialize_items();

  // 3. checksum item is tag(1) + len(1) + value(2) = 4 bytes of the value field.
  const std::uint64_t value_len = items.size() + 4;

  ber::Bytes pkt;
  for (auto b : ul_key) pkt.push_back(static_cast<std::byte>(b));
  ber::write_length(pkt, value_len);
  pkt.insert(pkt.end(), items.begin(), items.end());
  ber::write_oid(pkt, kChecksumTag);  // 0x01
  ber::write_length(pkt, 2);          // 0x02

  // 4. 16-bit BCC over key..checksum-length, then append the value (ST 0601 §6.6).
  const std::uint16_t cs = codec::bcc16(pkt);
  pkt.push_back(static_cast<std::byte>((cs >> 8) & 0xFF));
  pkt.push_back(static_cast<std::byte>(cs & 0xFF));
  return Result<ber::Bytes>::ok(std::move(pkt));
}

}  // namespace misbklv
