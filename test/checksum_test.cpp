// SPDX-License-Identifier: Apache-2.0
// Message::checksum_valid (issue #82): parse does not verify the Item 1
// checksum; checksum_valid() reports whether the source packet's matches.
// Valid packets are the committed fixtures, whose checksums come from the
// independent Python BCC16 in test/fixtures/, not from the library's bcc16.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

#include "misbklv/message.hpp"
#include "misbklv/packet.hpp"

using namespace misbklv;

static int failures = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("  [FAIL] %s\n", what);
    ++failures;
  }
}

static std::vector<std::byte> read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<std::byte> out;
  for (char x : c) out.push_back(static_cast<std::byte>(x));
  return out;
}

// 1 valid, 0 invalid, -1 checksum_valid errored, -2 parse errored.
static int valid_of(const std::vector<std::byte>& bytes) {
  auto m = Message::parse(bytes);
  if (!m) return -2;
  auto r = m->checksum_valid();
  return r ? (*r ? 1 : 0) : -1;
}

static Error err_of(const std::vector<std::byte>& bytes) {
  auto m = Message::parse(bytes);
  if (!m) return Error::Truncated;  // parse failure: never the expected error
  auto r = m->checksum_valid();
  return r ? Error::Truncated : r.error();
}

// key + BER length + body (caller supplies whole body incl. any tag 1 item).
static std::vector<std::byte> packet(std::span<const std::uint8_t> key,
                                     std::initializer_list<int> body) {
  std::vector<std::byte> p;
  for (auto c : key) p.push_back(static_cast<std::byte>(c));
  p.push_back(static_cast<std::byte>(body.size()));
  for (int c : body) p.push_back(static_cast<std::byte>(c));
  return p;
}

static void check_fixture(const std::vector<std::byte>& good, const char* name) {
  check(!good.empty(), name);
  check(valid_of(good) == 1, name);

  auto bad_cs = good;
  bad_cs[bad_cs.size() - 1] ^= std::byte{0xFF};
  bad_cs[bad_cs.size() - 2] ^= std::byte{0xFF};
  check(valid_of(bad_cs) == 0, "flipped checksum bytes are invalid (parse still ok)");

  auto bad_payload = good;
  bad_payload[bad_payload.size() - 8] ^= std::byte{0x01};  // inside an item value
  check(valid_of(bad_payload) == 0, "payload flip with stale checksum is invalid");
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const auto uas = read_file(argv[1]);   // synthetic-first-packet.klv (0601)
  const auto vmti = read_file(argv[2]);  // vmti_standalone.klv (0903)
  check_fixture(uas, "0601 fixture: checksum valid");
  check_fixture(vmti, "standalone VMTI fixture: checksum valid");

  // Edits are ignored: still validates the source packet.
  auto m = Message::parse(uas);
  check(m && m->set(143, Value{std::span<const std::byte>(uas.data(), 4)}) && *m->checksum_valid(),
        "staged edit does not change the verdict");

  // No checksum item.
  const std::span<const std::uint8_t> k0601(kUas0601Key);
  auto no_cs = packet(k0601, {0x0A, 0x01, 0x00});
  check(err_of(no_cs) == Error::UnknownTag, "missing checksum -> UnknownTag");

  // No source packet (create()): nothing to verify.
  auto created = Message::create(RegistryId::Uas0601);
  check(created && created->checksum_valid().error() == Error::UnknownTag,
        "created message -> UnknownTag");

  // Tag 1 value not 2 bytes wide (final item, 3-byte value).
  check(err_of(packet(k0601, {0x01, 0x03, 0x00, 0x00, 0x00})) == Error::BadLength,
        "tag 1 wrong width -> BadLength");

  // Well-formed 2-byte checksum item that is not the final item.
  check(err_of(packet(k0601, {0x01, 0x02, 0x00, 0x00, 0x0A, 0x01, 0x00})) == Error::BadLength,
        "checksum not final -> BadLength");
  return failures ? 1 : 0;
}
