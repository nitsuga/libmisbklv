// SPDX-License-Identifier: Apache-2.0
#include "misbklv/mdarray.hpp"

#include <bit>
#include <limits>

#include "misbklv/ber.hpp"
#include "misbklv/codec.hpp"

namespace misbklv::mdarray {
namespace {

bool mul(std::size_t a, std::size_t b, std::size_t& out) {
  if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) return false;
  out = a * b;
  return true;
}

Result<ber::Parsed> read(std::span<const std::byte> value, std::size_t& pos) {
  auto parsed = ber::read_oid(value, pos);
  if (parsed) pos += parsed->consumed;
  return parsed;
}

Result<std::span<const std::byte>> element_bytes(const MdArray& array, std::size_t flat_index,
                                                 Apa required, std::size_t min_width,
                                                 std::size_t max_width) {
  if (array.apa != required) return Result<std::span<const std::byte>>::err(Error::TypeMismatch);
  if (array.ebytes < min_width || array.ebytes > max_width)
    return Result<std::span<const std::byte>>::err(Error::BadLength);
  if (flat_index >= array.element_count())
    return Result<std::span<const std::byte>>::err(Error::OutOfRange);
  std::size_t offset{};
  if (!mul(flat_index, array.ebytes, offset) || offset > array.elements.size() ||
      array.ebytes > array.elements.size() - offset)
    return Result<std::span<const std::byte>>::err(Error::BadLength);
  return Result<std::span<const std::byte>>::ok(array.elements.subspan(offset, array.ebytes));
}

double ieee(std::span<const std::byte> bytes) {
  if (bytes.size() == 4) {
    const auto bits = static_cast<std::uint32_t>(codec::rd_uint(bytes));
    return std::bit_cast<float>(bits);
  }
  return std::bit_cast<double>(codec::rd_uint(bytes));
}

}  // namespace

std::size_t MdArray::element_count() const {
  std::size_t count = 1;
  for (auto dim : dims) {
    if (dim > std::numeric_limits<std::size_t>::max() ||
        !mul(count, static_cast<std::size_t>(dim), count))
      return 0;
  }
  return count;
}

Result<std::uint64_t> MdArray::element_uint(std::size_t flat_index) const {
  auto bytes = element_bytes(*this, flat_index, Apa::Natural, 1, 8);
  if (!bytes) return Result<std::uint64_t>::err(bytes.error());
  return Result<std::uint64_t>::ok(codec::rd_uint(*bytes));
}

Result<double> MdArray::element_float(std::size_t flat_index) const {
  auto bytes = element_bytes(*this, flat_index, Apa::Natural, 4, 8);
  if (!bytes) return Result<double>::err(bytes.error());
  if (bytes->size() != 4 && bytes->size() != 8) return Result<double>::err(Error::BadLength);
  return Result<double>::ok(ieee(*bytes));
}

Result<double> MdArray::element_imapb(std::size_t flat_index) const {
  auto bytes = element_bytes(*this, flat_index, Apa::Imapb, 1, 8);
  if (!bytes) return Result<double>::err(bytes.error());
  if (apas.size() != 8 && apas.size() != 16) return Result<double>::err(Error::BadLength);
  const std::size_t half = apas.size() / 2;
  ItemDescriptor descriptor{};
  descriptor.kind = ValueKind::IMAPB;
  descriptor.map = {ieee(apas.first(half)), ieee(apas.subspan(half))};
  return Result<double>::ok(codec::imapb_decode(descriptor, *bytes));
}

Result<MdArray> parse(std::span<const std::byte> value) {
  std::size_t pos = 0;
  auto ndim = read(value, pos);
  if (!ndim) return Result<MdArray>::err(ndim.error());
  if (ndim->value == 0) return Result<MdArray>::err(Error::BadLength);
  if (ndim->value > value.size() - pos) return Result<MdArray>::err(Error::BadLength);

  MdArray array;
  std::size_t count = 1;
  for (std::uint64_t i = 0; i < ndim->value; ++i) {
    auto dim = read(value, pos);
    if (!dim) return Result<MdArray>::err(dim.error());
    if (dim->value == 0 || dim->value > std::numeric_limits<std::size_t>::max())
      return Result<MdArray>::err(Error::BadLength);
    if (!mul(count, static_cast<std::size_t>(dim->value), count))
      return Result<MdArray>::err(Error::OutOfRange);
    array.dims.push_back(dim->value);
  }
  auto ebytes = read(value, pos);
  if (!ebytes) return Result<MdArray>::err(ebytes.error());
  if (ebytes->value > std::numeric_limits<std::uint32_t>::max())
    return Result<MdArray>::err(Error::OutOfRange);
  array.ebytes = static_cast<std::uint32_t>(ebytes->value);
  auto apa = read(value, pos);
  if (!apa) return Result<MdArray>::err(apa.error());
  if (apa->value < static_cast<std::uint8_t>(Apa::Natural) ||
      apa->value > static_cast<std::uint8_t>(Apa::RunLength))
    return Result<MdArray>::err(Error::BadLength);
  array.apa = static_cast<Apa>(apa->value);

  const auto remaining = value.subspan(pos);
  std::size_t fixed_elements{};
  switch (array.apa) {
    case Apa::Natural:
      if (!mul(count, array.ebytes, fixed_elements) || remaining.size() != fixed_elements)
        return Result<MdArray>::err(Error::BadLength);
      array.elements = remaining;
      break;
    case Apa::Imapb:
      if (!mul(count, array.ebytes, fixed_elements) || remaining.size() < fixed_elements)
        return Result<MdArray>::err(Error::BadLength);
      array.apas = remaining.first(remaining.size() - fixed_elements);
      if (array.apas.size() != 8 && array.apas.size() != 16)
        return Result<MdArray>::err(Error::BadLength);
      array.elements = remaining.subspan(array.apas.size());
      break;
    case Apa::BooleanCompact:
      if (array.ebytes != 0 && array.ebytes != 1) return Result<MdArray>::err(Error::BadLength);
      fixed_elements = count / 8 + (count % 8 != 0);
      if (array.ebytes == 0) fixed_elements = 0;
      if (remaining.size() != fixed_elements) return Result<MdArray>::err(Error::BadLength);
      if (array.ebytes != 0 && count % 8 != 0 &&
          (std::to_integer<unsigned char>(remaining.back()) & ((1u << (8 - count % 8)) - 1)) != 0)
        return Result<MdArray>::err(Error::BadLength);
      array.elements = remaining;
      break;
    case Apa::UintCompact: {
      if (array.ebytes != 0 && array.ebytes != 1) return Result<MdArray>::err(Error::BadLength);
      std::size_t apas_pos = 0;
      auto bias = read(remaining, apas_pos);
      if (!bias) return Result<MdArray>::err(bias.error());
      array.apas = remaining.first(apas_pos);
      array.elements =
          array.ebytes == 0 ? std::span<const std::byte>{} : remaining.subspan(apas_pos);
      if (array.ebytes == 0 && apas_pos != remaining.size())
        return Result<MdArray>::err(Error::BadLength);
      break;
    }
    case Apa::RunLength:
      if (remaining.size() < array.ebytes) return Result<MdArray>::err(Error::BadLength);
      array.apas = remaining.first(array.ebytes);
      array.elements = remaining.subspan(array.ebytes);
      if (array.ebytes == 0 && !array.elements.empty())
        return Result<MdArray>::err(Error::BadLength);
      break;
  }
  return Result<MdArray>::ok(array);
}

}  // namespace misbklv::mdarray
