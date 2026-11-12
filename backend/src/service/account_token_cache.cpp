#include "service/account_token_cache.h"

namespace tnycl::account {

LruTokenCache::LruTokenCache(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

bool LruTokenCache::expired(const Entry& entry) {
  return std::chrono::steady_clock::now() >= entry.expires_at;
}

std::optional<Principal> LruTokenCache::get(std::string_view token_hash) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto it = index_.find(std::string(token_hash));
  if (it == index_.end()) {
    return std::nullopt;
  }
  if (expired(it->second->second)) {
    items_.erase(it->second);
    index_.erase(it);
    return std::nullopt;
  }
  items_.splice(items_.begin(), items_, it->second);
  return it->second->second.principal;
}

void LruTokenCache::put(const std::string& token_hash, const Principal& principal,
                        int ttl_seconds) {
  if (ttl_seconds <= 0) {
    return;
  }
  std::lock_guard<std::mutex> guard(mutex_);
  const auto existing = index_.find(token_hash);
  if (existing != index_.end()) {
    existing->second->second.principal = principal;
    existing->second->second.expires_at =
        std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds);
    items_.splice(items_.begin(), items_, existing->second);
    return;
  }
  items_.emplace_front(token_hash, Entry{principal, std::chrono::steady_clock::now() +
                                                        std::chrono::seconds(ttl_seconds)});
  index_[token_hash] = items_.begin();
  if (index_.size() > capacity_) {
    const auto& victim = items_.back();
    index_.erase(victim.first);
    items_.pop_back();
  }
}

void LruTokenCache::erase(std::string_view token_hash) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto it = index_.find(std::string(token_hash));
  if (it == index_.end()) {
    return;
  }
  items_.erase(it->second);
  index_.erase(it);
}

}  // namespace tnycl::account
