// adapters/argon2_hasher.cpp —— Argon2id 密码哈希实现（T-3.02）
#include "adapters/argon2_hasher.h"

#include <random>
#include <stdexcept>

#ifdef TNYCL_HAVE_ARGON2
#include <argon2.h>
#endif

namespace tnycl {
namespace {

constexpr char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace

std::string base64_encode(std::string_view raw) {
  std::string out;
  out.reserve(((raw.size() + 2) / 3) * 4);
  for (std::size_t i = 0; i < raw.size(); i += 3) {
    std::uint32_t n = static_cast<std::uint32_t>(
        static_cast<std::uint8_t>(raw[i])) << 16;
    if (i + 1 < raw.size())
      n |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(raw[i + 1])) << 8;
    if (i + 2 < raw.size())
      n |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(raw[i + 2]));
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += (i + 1 < raw.size()) ? kB64[(n >> 6) & 63] : '=';
    out += (i + 2 < raw.size()) ? kB64[n & 63] : '=';
  }
  return out;
}

std::vector<std::uint8_t> base64_decode(std::string_view b64) {
  std::vector<std::uint8_t> out;
  std::uint32_t n = 0;
  int bits = 0;
  for (char c : b64) {
    if (c == '=') break;
    int v = b64_value(c);
    if (v < 0) return {};
    n = (n << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((n >> bits) & 0xFF));
    }
  }
  return out;
}

std::vector<std::uint8_t> random_salt(std::size_t len) {
  std::vector<std::uint8_t> salt(len);
  std::random_device rd;
  for (auto& b : salt) b = static_cast<std::uint8_t>(rd());
  return salt;
}

std::string phc_encode(std::uint32_t m, std::uint32_t t, std::uint32_t p,
                       std::string_view salt, std::string_view hash) {
  std::string out = "$argon2id$v=19$m=" + std::to_string(m) +
                    ",t=" + std::to_string(t) + ",p=" + std::to_string(p) +
                    "$" + base64_encode(salt) + "$" + base64_encode(hash);
  return out;
}

Phc phc_decode(std::string_view phc) {
  Phc r;
  // 按 '$' 分段：["", "argon2id", "v=19", "m=...,t=...,p=...", salt, hash]
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  for (;;) {
    std::size_t pos = phc.find('$', start);
    if (pos == std::string_view::npos) {
      parts.push_back(phc.substr(start));
      break;
    }
    parts.push_back(phc.substr(start, pos - start));
    start = pos + 1;
  }
  if (parts.size() != 6) return r;
  if (parts[1] != "argon2id") return r;
  if (parts[2] != "v=19") return r;

  bool have_m = false, have_t = false, have_p = false;
  std::size_t p = 0;
  for (;;) {
    std::size_t q = parts[3].find(',', p);
    std::string_view kv = (q == std::string_view::npos) ? parts[3].substr(p)
                                                        : parts[3].substr(p, q - p);
    if (kv.size() < 3 || kv[1] != '=') return r;
    int val = 0;
    for (std::size_t i = 2; i < kv.size(); ++i) {
      if (kv[i] < '0' || kv[i] > '9') return r;
      val = val * 10 + (kv[i] - '0');
    }
    if (kv[0] == 'm') {
      r.m = static_cast<std::uint32_t>(val);
      have_m = true;
    } else if (kv[0] == 't') {
      r.t = static_cast<std::uint32_t>(val);
      have_t = true;
    } else if (kv[0] == 'p') {
      r.p = static_cast<std::uint32_t>(val);
      have_p = true;
    } else {
      return r;
    }
    if (q == std::string_view::npos) break;
    p = q + 1;
  }
  if (!have_m || !have_t || !have_p) return r;

  r.salt = base64_decode(parts[4]);
  r.hash = base64_decode(parts[5]);
  if (r.salt.empty() || r.hash.empty()) return r;
  r.ok = true;
  return r;
}

bool secure_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i)
    diff |= static_cast<std::uint8_t>(a[i]) ^ static_cast<std::uint8_t>(b[i]);
  return diff == 0;
}

std::string Argon2idHasher::hash(std::string_view password) const {
  auto salt = random_salt(16);
  std::vector<std::uint8_t> out(params_.hash_len);
  raw_(reinterpret_cast<const std::uint8_t*>(password.data()), password.size(),
       salt.data(), salt.size(), params_.m_cost_kib, params_.t_cost,
       params_.lanes, out.data(), out.size());
  return phc_encode(params_.m_cost_kib, params_.t_cost, params_.lanes,
                    std::string_view(reinterpret_cast<const char*>(salt.data()),
                                     salt.size()),
                    std::string_view(reinterpret_cast<const char*>(out.data()),
                                     out.size()));
}

bool Argon2idHasher::verify(std::string_view password, std::string_view phc) const {
  Phc p = phc_decode(phc);
  if (!p.ok) return false;
  if (p.m != params_.m_cost_kib || p.t != params_.t_cost || p.p != params_.lanes)
    return false;
  if (p.hash.size() != params_.hash_len) return false;
  std::vector<std::uint8_t> out(params_.hash_len);
  raw_(reinterpret_cast<const std::uint8_t*>(password.data()), password.size(),
       p.salt.data(), p.salt.size(), params_.m_cost_kib, params_.t_cost,
       params_.lanes, out.data(), out.size());
  return secure_equal(
      std::string_view(reinterpret_cast<const char*>(out.data()), out.size()),
      std::string_view(reinterpret_cast<const char*>(p.hash.data()),
                       p.hash.size()));
}

#ifdef TNYCL_HAVE_ARGON2
// 生产构造：绑定 libargon2。参数 t/m/p 与 config.cpu 对齐。
Argon2idHasher::Argon2idHasher(Params params) : params_(params) {
  raw_ = [](const std::uint8_t* pw, std::size_t pwlen, const std::uint8_t* salt,
            std::size_t saltlen, std::uint32_t m, std::uint32_t t,
            std::uint32_t lanes, std::uint8_t* out, std::size_t outlen) {
    int rc = argon2id_hash_raw(t, m, lanes, pw, pwlen, salt, saltlen, out, outlen);
    if (rc != ARGON2_OK)
      throw std::runtime_error(std::string("argon2id_hash_raw: ") +
                               argon2_error_message(rc));
  };
}
#else
// 无 libargon2 环境：不绑定原语。仅用于在缺库环境编译通过；调用 hash/verify 会抛
// std::bad_function_call。生产（Ubuntu + libargon2-dev）走上面的分支。
Argon2idHasher::Argon2idHasher(Params params) : params_(params) {}
#endif

}  // namespace tnycl
