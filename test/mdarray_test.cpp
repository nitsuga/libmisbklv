// SPDX-License-Identifier: Apache-2.0
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "misbklv/ber.hpp"
#include "misbklv/codec.hpp"
#include "misbklv/mdarray.hpp"

using namespace misbklv;

static int failures = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("  %-42s FAIL\n", what);
    ++failures;
  }
}

static ber::Bytes pack(std::initializer_list<std::uint64_t> dims, std::uint64_t ebytes,
                       std::uint64_t apa, std::span<const std::byte> tail) {
  ber::Bytes out;
  ber::write_oid(out, dims.size());
  for (auto dim : dims) ber::write_oid(out, dim);
  ber::write_oid(out, ebytes);
  ber::write_oid(out, apa);
  out.insert(out.end(), tail.begin(), tail.end());
  return out;
}

static ber::Bytes pack(std::initializer_list<std::uint64_t> dims, std::uint64_t ebytes,
                       std::uint64_t apa, std::initializer_list<std::byte> tail) {
  return pack(dims, ebytes, apa, std::span<const std::byte>(tail.begin(), tail.size()));
}

static void append_float(ber::Bytes& out, float value) {
  std::uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  out.push_back(static_cast<std::byte>(bits >> 24));
  out.push_back(static_cast<std::byte>(bits >> 16));
  out.push_back(static_cast<std::byte>(bits >> 8));
  out.push_back(static_cast<std::byte>(bits));
}

int main() {
  {
    auto raw = pack({2, 2}, 2, 1, {std::byte{0}, std::byte{1}, std::byte{0}, std::byte{2},
                                    std::byte{0}, std::byte{3}, std::byte{0}, std::byte{4}});
    auto parsed = mdarray::parse(raw);
    check(parsed && parsed->element_count() == 4 && parsed->has_data(), "Natural shape/data");
    auto item = parsed ? parsed->element_uint(2) : Result<std::uint64_t>::err(Error::BadLength);
    check(item && *item == 3, "Natural uint element");
    auto out = parsed ? parsed->element_uint(4) : Result<std::uint64_t>::err(Error::BadLength);
    check(!out && out.error() == Error::OutOfRange, "Natural index bound");
  }
  {
    ber::Bytes tail;
    append_float(tail, 1.5f);
    auto raw = pack({1}, 4, 1, tail);
    auto parsed = mdarray::parse(raw);
    auto item = parsed ? parsed->element_float(0) : Result<double>::err(Error::BadLength);
    check(item && std::fabs(*item - 1.5) < 1e-6, "Natural float element");
  }
  {
    ItemDescriptor d{};
    d.kind = ValueKind::IMAPB;
    d.map = {0.0, 100.0};
    ber::Bytes mapped;
    codec::imapb_encode(d, 25.0, mapped, 2);
    ber::Bytes tail;
    append_float(tail, 0.0f);
    append_float(tail, 100.0f);
    tail.insert(tail.end(), mapped.begin(), mapped.end());
    auto raw = pack({1}, 2, 2, tail);
    auto parsed = mdarray::parse(raw);
    auto item = parsed ? parsed->element_imapb(0) : Result<double>::err(Error::BadLength);
    check(item && std::fabs(*item - 25.0) < 0.01, "IMAPB element");
  }
  {
    auto raw = pack({5}, 1, 3, {std::byte{0xA8}});
    auto parsed = mdarray::parse(raw);
    check(parsed && parsed->elements.size() == 1, "Boolean compact shape");
    auto bad_padding = pack({5}, 1, 3, {std::byte{0xAF}});
    check(!mdarray::parse(bad_padding), "Boolean nonzero padding rejected");
  }
  {
    ber::Bytes tail;
    ber::write_oid(tail, 0);  // required UInt-compact bias
    ber::write_oid(tail, 7);
    auto raw = pack({1}, 1, 4, tail);
    auto parsed = mdarray::parse(raw);
    check(parsed && parsed->apas.size() == 1 && parsed->elements.size() == 1, "UInt compact split");
  }
  {
    auto raw = pack({1}, 2, 5, {std::byte{0}, std::byte{0}});
    auto parsed = mdarray::parse(raw);
    check(parsed && parsed->apas.size() == 2 && parsed->elements.empty() && parsed->has_data(),
          "Run-length default split");
  }
  {
    auto raw = pack({4}, 0, 1, {});
    auto parsed = mdarray::parse(raw);
    check(parsed && !parsed->has_data() && parsed->element_count() == 4, "Empty Natural array");
    auto compact = pack({5}, 0, 3, {});
    auto compact_parsed = mdarray::parse(compact);
    check(compact_parsed && !compact_parsed->has_data(), "Empty Boolean array");
  }
  {
    auto short_natural = pack({2}, 2, 1, {std::byte{0}});
    auto bad_imapb = pack({1}, 1, 2, {std::byte{0}});
    auto bad_boolean = pack({9}, 1, 3, {std::byte{0}});
    auto bad_apa = pack({1}, 1, 6, {std::byte{0}});
    check(!mdarray::parse(short_natural), "Natural short payload rejected");
    check(!mdarray::parse(bad_imapb), "IMAPB APAS rejected");
    check(!mdarray::parse(bad_boolean), "Boolean short payload rejected");
    check(!mdarray::parse(bad_apa), "Unknown APA rejected");
  }
  {
    auto overflow = pack({std::numeric_limits<std::size_t>::max(), 2}, 1, 1, {});
    check(!mdarray::parse(overflow), "Dimension product overflow rejected");
  }
  return failures ? 1 : 0;
}
