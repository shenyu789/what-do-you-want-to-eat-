// M3 测试替身：内存实现只存在于测试目录，不进入生产代码，避免复制 Agent A 的基础设施。
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/account_repository.h"
#include "service/account_service.h"
#include "tnycl/error_codes.h"
#include "tnycl/status.h"

namespace tnycl::test {

class FakeClock final : public account::Clock {
 public:
  std::int64_t now{1'700'000'000'000};

  std::int64_t now_ms() const override { return now; }
};

class FakeHasher final : public account::PasswordHasher {
 public:
  std::string hash(std::string_view password) override {
    return std::string("$argon2id$fake$") + std::string(password);
  }
  bool verify(std::string_view password, std::string_view pass_hash) override {
    return pass_hash == std::string("$argon2id$fake$") + std::string(password);
  }
};

class FakeTokenSource final : public account::TokenSource {
 public:
  std::string generate() override { return "token-" + std::to_string(++counter_); }
  std::string sha256_hex(std::string_view token) override {
    return "sha256:" + std::string(token);
  }

 private:
  int counter_{0};
};

class FakeUserRepository final : public account::UserRepository {
 public:
  std::optional<account::UserRecord> find_by_username(std::string_view username) override {
    for (const auto& [id, user] : users_) {
      if (user.username == username) {
        return user;
      }
    }
    return std::nullopt;
  }

  std::optional<account::UserRecord> find_by_id(std::uint64_t user_id) override {
    const auto it = users_.find(user_id);
    if (it == users_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  tnycl::Result<std::uint64_t> insert(std::string_view username, std::string_view pass_hash,
                                      std::int64_t now_ms) override {
    if (find_by_username(username).has_value()) {
      return tnycl::Result<std::uint64_t>::err(tnycl::ErrorCode::kUsernameTaken);
    }
    const std::uint64_t id = next_id_++;
    account::UserRecord record;
    record.user_id = id;
    record.username = std::string(username);
    record.pass_hash = std::string(pass_hash);
    record.created_at_ms = now_ms;
    users_[id] = record;
    return tnycl::Result<std::uint64_t>::ok(id);
  }

  tnycl::Result<void> update_login_state(std::uint64_t user_id, int fail_count,
                                         std::optional<std::int64_t> locked_until_ms) override {
    const auto it = users_.find(user_id);
    if (it == users_.end()) {
      return tnycl::Result<void>::err(tnycl::ErrorCode::kInternalError, "用户不存在");
    }
    it->second.fail_count = fail_count;
    it->second.locked_until_ms = locked_until_ms;
    return tnycl::Result<void>::ok();
  }

  void set_status(std::uint64_t user_id, int status) { users_[user_id].status = status; }
  std::size_t size() const { return users_.size(); }

 private:
  std::uint64_t next_id_{1};
  std::map<std::uint64_t, account::UserRecord> users_;
};

class FakeSessionRepository final : public account::SessionRepository {
 public:
  tnycl::Result<std::uint64_t> insert(std::uint64_t user_id, std::string_view token_hash,
                                      std::string_view device, std::int64_t created_at_ms,
                                      std::int64_t expires_at_ms) override {
    account::SessionRecord record;
    record.session_id = sessions_.size() + 1;
    record.user_id = user_id;
    record.token_hash = std::string(token_hash);
    record.device = std::string(device);
    record.created_at_ms = created_at_ms;
    record.expires_at_ms = expires_at_ms;
    sessions_.push_back(record);
    return tnycl::Result<std::uint64_t>::ok(record.session_id);
  }

  std::optional<account::SessionRecord> find_by_token_hash(std::string_view token_hash) override {
    for (const auto& session : sessions_) {
      if (session.token_hash == token_hash) {
        return session;
      }
    }
    return std::nullopt;
  }

  // 与 MySQL 实现一致：登出即删除会话行，重复吊销幂等。
  tnycl::Result<void> revoke(std::string_view token_hash, std::int64_t) override {
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                   [&](const account::SessionRecord& session) {
                                     return session.token_hash == token_hash;
                                   }),
                    sessions_.end());
    return tnycl::Result<void>::ok();
  }

  std::vector<account::SessionRecord> sessions_;
};

class FakeProfileLookup final : public account::ProfileLookup {
 public:
  tnycl::Result<bool> has_profile(std::uint64_t) override {
    return tnycl::Result<bool>::ok(has_profile_);
  }

  bool has_profile_{false};
};

// 记录调用次数，用于验证"账号不存在也执行一次哈希校验"。
class CountingHasher final : public account::PasswordHasher {
 public:
  std::string hash(std::string_view password) override {
    ++hash_calls;
    return std::string("$argon2id$fake$") + std::string(password);
  }
  bool verify(std::string_view password, std::string_view pass_hash) override {
    ++verify_calls;
    return pass_hash == std::string("$argon2id$fake$") + std::string(password);
  }

  int hash_calls{0};
  int verify_calls{0};
};

}  // namespace tnycl::test
