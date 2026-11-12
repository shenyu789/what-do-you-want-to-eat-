// tnycl/redis_client.h —— Redis 客户端接口（W2 冻结契约）
//
// 语义约定：
//  - Redis 仅作缓存/加速，永不作权威；Redis 不可用时所有方法安全失败（返回 nullopt/false），
//    调用方降级到 MySQL，绝不因此抛异常导致请求失败（见 02 §9）。
//  - 只允许 GET/SET/INCR/DEL/ZADD/ZREVRANGE/EXPIRE 等，严禁 SUBSCRIBE/PUBLISH/KEYS/FLUSHALL。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tnycl {

class IRedisClient {
 public:
  virtual ~IRedisClient() = default;

  // Redis 当前是否可用（连接正常返回 true）。
  virtual bool is_available() const noexcept = 0;

  virtual std::optional<std::string> get(const std::string& key) = 0;

  // 成功返回 true；ttl_sec 为 nullopt 表示不设过期。
  virtual bool set(const std::string& key, const std::string& value,
                   std::optional<int> ttl_sec = std::nullopt) = 0;

  // 自增，返回自增后的值；失败返回 nullopt。
  virtual std::optional<long long> incr(const std::string& key) = 0;

  virtual bool del(const std::string& key) = 0;
  virtual bool expire(const std::string& key, int ttl_sec) = 0;

  // 有序集合（榜单加速）。
  virtual bool zadd(const std::string& key, double score,
                    const std::string& member) = 0;
  virtual std::vector<std::string> zrevrange(const std::string& key,
                                             long start, long stop) = 0;
};

}  // namespace tnycl
