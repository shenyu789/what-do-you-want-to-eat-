// adapters/argon2_hasher.h —— Argon2id 密码哈希适配器（T-3.02）
//
// 实现 service/account_repository.h 的 PasswordHasher 端口。
// 输出 PHC 字符串：$argon2id$v=19$m=65536,t=3,p=1$<b64salt>$<b64hash>
//   - m 单位 KiB（65536 = 64 MiB），与 config.cpu.argon2_memory_kb 一致。
//   - 参数固定，verify 时 PHC 中的参数必须与配置一致，否则拒绝（防参数降级攻击）。
// 生产构造绑定 argon2.h 的 argon2id_hash_raw（需 libargon2，Ubuntu 编译）；测试可注入伪原语。
// 本类自身不感知线程：调用方（Service）负责投递到 CPU 池，禁止在 IO 线程执行（NFR-S-02）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tnycl {

// 底层 Argon2id 计算原语（生产 = argon2id_hash_raw；测试 = 注入伪实现）。
using Argon2RawFn = std::function<void(const std::uint8_t* pw, std::size_t pwlen,
                                       const std::uint8_t* salt, std::size_t saltlen,
                                       std::uint32_t m_cost_kib, std::uint32_t t_cost,
                                       std::uint32_t lanes, std::uint8_t* out,
                                       std::size_t outlen)>;

class Argon2idHasher {
 public:
  struct Params {
    std::uint32_t m_cost_kib = 65536;  // 64 MiB
    std::uint32_t t_cost = 3;
    std::uint32_t lanes = 1;
    std::size_t hash_len = 32;
  };

  explicit Argon2idHasher(Params params, Argon2RawFn raw)
      : params_(params), raw_(std::move(raw)) {}

  // 生产构造：绑定 libargon2 的 argon2id_hash_raw（在 .cpp 中定义）。
  explicit Argon2idHasher(Params params);

  std::string hash(std::string_view password) const;
  bool verify(std::string_view password, std::string_view phc) const;

 private:
  Params params_;
  Argon2RawFn raw_;
};

// ---- 纯函数（无 IO、无 lib 依赖，可单测）----

// 生成随机盐（默认 16 字节；用 std::random_device，安全随机）。
std::vector<std::uint8_t> random_salt(std::size_t len = 16);

// RFC 4648 标准 base64（带 '=' 填充）。
std::string base64_encode(std::string_view raw);
// 解码失败返回空 vector。
std::vector<std::uint8_t> base64_decode(std::string_view b64);

// PHC 编码：$argon2id$v=19$m=...,t=...,p=...$<b64salt>$<b64hash>
std::string phc_encode(std::uint32_t m, std::uint32_t t, std::uint32_t p,
                       std::string_view salt, std::string_view hash);

struct Phc {
  bool ok = false;
  std::uint32_t m = 0;
  std::uint32_t t = 0;
  std::uint32_t p = 0;
  std::vector<std::uint8_t> salt;
  std::vector<std::uint8_t> hash;
};

// PHC 解码：格式非法返回 ok=false。
Phc phc_decode(std::string_view phc);

// 常量时间比较（防时序侧信道）。
bool secure_equal(std::string_view a, std::string_view b);

}  // namespace tnycl
