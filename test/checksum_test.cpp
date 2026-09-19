// SPDX-License-Identifier: Apache-2.0
// Message::checksum_valid (issue #82): parse does not verify the ST 0601
// checksum; checksum_valid() reports whether the source packet's matches.
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "misbklv/builder.hpp"
#include "misbklv/message.hpp"
#include "misbklv/registry/uas0601_tables.generated.hpp"

using namespace misbklv;

static int failures = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("  [FAIL] %s\n", what);
    ++failures;
  }
}

static std::vector<std::byte> make_packet() {
  const std::vector<std::byte> payload(4, std::byte{0xAB});
  LocalSetBuilder b(gen::uas_0601);
  b.append_raw(143, payload);
  auto pkt = std::move(b).finalize(std::span{kUas0601Key}, /*enforce=*/false);
  return pkt ? *pkt : std::vector<std::byte>{};
}

// Returns valid state, or -1 if parse/checksum_valid errored.
static int valid_of(const std::vector<std::byte>& bytes) {
  auto m = Message::parse(bytes);
  if (!m) return -2;
  auto r = m->checksum_valid();
  return r ? (*r ? 1 : 0) : -1;
}

int main() {
  const auto good = make_packet();
  check(!good.empty(), "build valid packet");
  check(valid_of(good) == 1, "untouched packet is valid");

  auto bad_cs = good;
  bad_cs[bad_cs.size() - 1] ^= std::byte{0xFF};
  bad_cs[bad_cs.size() - 2] ^= std::byte{0xFF};
  check(valid_of(bad_cs) == 0, "flipped checksum bytes are invalid (parse still ok)");

  auto bad_payload = good;
  bad_payload[bad_payload.size() - 8] ^= std::byte{0x01};  // inside tag 143 value
  check(valid_of(bad_payload) == 0, "payload flip with stale checksum is invalid");

  // Edits are ignored: still validates the source packet.
  auto m = Message::parse(good);
  check(m && m->set(143, Value{std::span<const std::byte>(good.data(), 4)}) && *m->checksum_valid(),
        "staged edit does not change the verdict");

  // No checksum item: bare packet without tag 1.
  std::vector<std::byte> no_cs;
  for (auto c : kUas0601Key) no_cs.push_back(static_cast<std::byte>(c));
  for (auto c : {0x03, 0x0A, 0x01, 0x00}) no_cs.push_back(static_cast<std::byte>(c));
  auto r = Message::parse(no_cs);
  check(r.operator bool(), "packet without checksum parses");
  if (r) {
    auto cv = r->checksum_valid();
    check(!cv && cv.error() == Error::UnknownTag, "missing checksum -> UnknownTag");
  }
  return failures ? 1 : 0;
}
