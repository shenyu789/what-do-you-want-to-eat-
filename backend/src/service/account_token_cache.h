// T-3.05 鉴权本地 LRU 缓存（32B TTL、10k 容量），Redis 故障时兜底（AC-M3-10）。
#pragma once

#include <chrono>
#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "service/account_repository.h"

namespace tnycl::account {

class LruTokenCache final : public TokenCache {
 public:
  explicit LruTokenCache(std::size_t capacity = 10000);

  std::optional<Principal> get(std::string_view token_hash) override;
  void put(const std::string& token_hash, const Principal& principal, int ttl_seconds) override;
  void erase(std::string_view token_hash) override;

 private:
  struct Entry {
    Principal principal;
    std::chrono::steady_clock::time_point expires_at;
  };
  using Item = std::pair<std::string, Entry>;

  static bool expired(const Entry& entry);

  std::mutex mutex_;
  std::size_t capacity_;
  std::list<Item> items_;  // 头部为最近使用
  std::unordered_map<std::string, std::list<Item>::iterator> index_;
};

}  // namespace tnycl::account
