// 极简测试骨架：无第三方依赖，供 M3/M4/M5 单测在 Agent A 的测试框架（T-1.05）落地前使用。
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace tnycl::test {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& cases() {
  static std::vector<Case> registry;
  return registry;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { cases().push_back({name, fn}); }
};

inline int& failures() {
  static int value = 0;
  return value;
}

inline int& checks() {
  static int value = 0;
  return value;
}

inline void report(bool passed, const char* expression, const char* file, int line) {
  ++checks();
  if (!passed) {
    ++failures();
    std::printf("  FAIL %s:%d  %s\n", file, line, expression);
  }
}

inline int run_all() {
  for (const auto& item : cases()) {
    const int before = failures();
    item.fn();
    std::printf("[%s] %s\n", failures() == before ? "PASS" : "FAIL", item.name);
  }
  std::printf("checks=%d failures=%d\n", checks(), failures());
  return failures() == 0 ? 0 : 1;
}

}  // namespace tnycl::test

#define TNYCL_TEST(name)                                              \
  static void name();                                                 \
  static ::tnycl::test::Registrar tnycl_reg_##name(#name, name);       \
  static void name()

#define CHECK(condition) \
  ::tnycl::test::report(static_cast<bool>(condition), #condition, __FILE__, __LINE__)

#define CHECK_EQ(actual, expected) \
  ::tnycl::test::report((actual) == (expected), #actual " == " #expected, __FILE__, __LINE__)

#define CHECK_NEAR(actual, expected)                                                   \
  ::tnycl::test::report(std::fabs(static_cast<double>(actual) - (expected)) < 1e-9,     \
                        #actual " ≈ " #expected, __FILE__, __LINE__)
