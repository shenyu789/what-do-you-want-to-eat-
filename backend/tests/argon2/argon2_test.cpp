// T-3.02 Argon2id 适配器测试：PHC 编解码 / base64 / verify 逻辑（注入伪原语，无需 libargon2）
#include <cstdint>
#include <string>
#include <vector>

#include "adapters/argon2_hasher.h"
#include "support/test_harness.h"

using namespace tnycl;

namespace {

// 确定性伪原语（仅验证编解码与 verify 逻辑，非密码学用途）。
void fake_raw(const std::uint8_t* pw, std::size_t pwlen, const std::uint8_t* salt,
              std::size_t saltlen, std::uint32_t, std::uint32_t, std::uint32_t,
              std::uint8_t* out, std::size_t outlen) {
  for (std::size_t i = 0; i < outlen; ++i)
    out[i] = static_cast<std::uint8_t>(pw[i % pwlen] ^ salt[i % saltlen] ^ i);
}

}  // namespace

TNYCL_TEST(base64_roundtrip) {
  std::string raw = "hello world 123";
  std::string enc = base64_encode(raw);
  auto dec = base64_decode(enc);
  CHECK_EQ(dec.size(), raw.size());
  CHECK(std::string(dec.begin(), dec.end()) == raw);
}

TNYCL_TEST(base64_decode_rejects_invalid) {
  CHECK(base64_decode("!!!invalid!!!").empty());
}

TNYCL_TEST(phc_roundtrip) {
  std::string salt = "somesalt";
  std::string hash = "somehash";
  std::string phc = phc_encode(65536, 3, 1, salt, hash);
  Phc p = phc_decode(phc);
  CHECK(p.ok);
  CHECK_EQ(p.m, 65536);
  CHECK_EQ(p.t, 3);
  CHECK_EQ(p.p, 1);
  CHECK(std::string(p.salt.begin(), p.salt.end()) == salt);
  CHECK(std::string(p.hash.begin(), p.hash.end()) == hash);
}

TNYCL_TEST(phc_decode_rejects_malformed) {
  CHECK(!phc_decode("not-a-phc").ok);
  CHECK(!phc_decode("$argon2id$v=19$m=65536,t=3,p=1$").ok);           // 缺 hash
  CHECK(!phc_decode("$bcrypt$v=19$m=1,t=1,p=1$abc$def").ok);          // 非 argon2id
  CHECK(!phc_decode("$argon2id$v=19$m=65536,t=3$c2FsdA$").ok);        // 缺 p 参数
}

TNYCL_TEST(hasher_hash_verify_correct) {
  Argon2idHasher hasher(Argon2idHasher::Params{}, fake_raw);
  std::string phc = hasher.hash("correct horse battery staple");
  Phc p = phc_decode(phc);
  CHECK(p.ok);
  CHECK(hasher.verify("correct horse battery staple", phc));
}

TNYCL_TEST(hasher_verify_wrong_password) {
  Argon2idHasher hasher(Argon2idHasher::Params{}, fake_raw);
  std::string phc = hasher.hash("right password");
  CHECK(!hasher.verify("wrong password", phc));
}

TNYCL_TEST(hasher_verify_malformed_hash) {
  Argon2idHasher hasher(Argon2idHasher::Params{}, fake_raw);
  CHECK(!hasher.verify("pw", "garbage"));
  CHECK(!hasher.verify("pw", ""));
}

TNYCL_TEST(hasher_verify_param_mismatch) {
  Argon2idHasher hasher(Argon2idHasher::Params{}, fake_raw);  // m=65536,t=3,p=1
  std::string weak = phc_encode(8, 1, 1, "salt", std::string(32, 'h'));
  CHECK(!hasher.verify("pw", weak));  // 参数降级 → 拒绝
}

TNYCL_TEST(random_salt_is_random) {
  auto a = random_salt(16);
  auto b = random_salt(16);
  CHECK_EQ(a.size(), 16);
  CHECK(a != b);
}

int main() { return tnycl::test::run_all(); }
