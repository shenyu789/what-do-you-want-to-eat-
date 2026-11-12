cmake_minimum_required(VERSION 3.28)

project(eat_backend LANGUAGES CXX VERSION 0.1.0)

# ============================================================================
# 工具链门禁（D-06 / T-06）：Ubuntu 24.04 + GCC 13 + C++20 + CMake >= 3.28
# 用其他编译器/EOL 标准会直接 FATAL_ERROR，避免"Windows 能编就当验收"。
# ============================================================================
if(CMAKE_VERSION VERSION_LESS 3.28)
  message(FATAL_ERROR "CMake >= 3.28 required, got ${CMAKE_VERSION}")
endif()

if(BUILD_WITH_STRICT_TOOLCHAIN)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "GCC required (D-06), got ${CMAKE_CXX_COMPILER_ID}")
  endif()
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
    message(FATAL_ERROR "GCC >= 13 required (D-06), got ${CMAKE_CXX_COMPILER_VERSION}")
  endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 线程池/定时器使用 std::thread，Linux 需要 -pthread
find_package(Threads REQUIRED)

# JSON：tnycl/api_response.h 依赖 <nlohmann/json.hpp>（07 §1.1 的响应包封）。
# 三种来源按序尝试，避免"本机能编、Ubuntu 缺包"：
#   ① 系统包 nlohmann-json3-dev（CI 安装，最快）
#   ② 配置期已有的 nlohmann_json target
#   ③ FetchContent 固定 tag（离线/无包管理器时的兜底，版本可复现）
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  # 只靠 tag 固定版本；**不写 URL_HASH**——手写哈希值必然是编造的，
  # 一旦与真实值不符会让构建以"哈希不匹配"失败且难以定位。
  # 若后续要加完整性校验，必须在真机上下载后 `sha256sum` 取值再填入。
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  message(STATUS "系统未安装 nlohmann_json，改用 FetchContent v3.11.3")
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# 告警按 03-WBS T-1.01 判据：-std=c++20 -Wall -Wextra -Werror。
# 不额外加 -Wconversion 等更严的开关——基线没有承诺的严格度不擅自加码。
add_library(eat_warnings INTERFACE)
target_compile_options(eat_warnings INTERFACE -Wall -Wextra -Werror)
target_compile_features(eat_warnings INTERFACE cxx_std_20)

# ============================================================================
# MySQL 客户端发现（T-2.04）：src/adapters/mysql_conn.cpp 需要 <mysql.h> + libmysqlclient。
#
# 为什么现在必须显式找库：CMakeLists 此前从未提过 MySQL，而 mysql_conn.cpp
# 又一直处于**未入库**状态 —— 也就是说这个适配层从未被任何一次 CI 编译过。
# 一旦把它纳入构建却仍不显式声明依赖，失败会以 "mysql.h: No such file" 的形式
# 指向源码，而不指向真正原因（缺 libmysqlclient-dev），排查成本极高。
#
# 查找顺序：pkg-config(mysqlclient) → find_path/find_library 兜底。
# 两者都失败时**不在这里报错**，只记录 EAT_MYSQL_FOUND=OFF；
# 是否致命由下方"核心库"块按 BUILD_WITH_STRICT_TOOLCHAIN 决定。
# ============================================================================
set(EAT_MYSQL_FOUND OFF)
set(EAT_MYSQL_LINK "")

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(MYSQLCLIENT QUIET IMPORTED_TARGET mysqlclient)
  if(TARGET PkgConfig::MYSQLCLIENT)
    set(EAT_MYSQL_FOUND ON)
    set(EAT_MYSQL_LINK PkgConfig::MYSQLCLIENT)
  endif()
endif()

if(NOT EAT_MYSQL_FOUND)
  # 兜底：直接用 find_path/find_library（无 pkg-config，或包未提供 .pc）
  find_path(MYSQL_INCLUDE_DIR NAMES mysql.h PATH_SUFFIXES mysql)
  find_library(MYSQL_LIBRARY NAMES mysqlclient mysqlclient_r)
  if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
    set(EAT_MYSQL_FOUND ON)
  endif()
endif()

if(EAT_MYSQL_FOUND)
  message(STATUS "libmysqlclient: 已找到（${EAT_MYSQL_LINK}${MYSQL_LIBRARY}）")
else()
  message(STATUS "libmysqlclient: 未找到")
endif()

# ============================================================================
# 核心库：src/ 下除 main.cpp 外的全部实现
# CONFIGURE_DEPENDS 让新增源文件不必手改本文件（W1 期间各 Agent 并行加文件）
# ============================================================================
file(GLOB_RECURSE EAT_CORE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM EAT_CORE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/empty_tu.cpp)   # 占位 TU 只在无实现时才用

# mysql_conn.cpp 是唯一依赖 <mysql.h> 的 TU。缺库时：
#   - 验收构建（BUILD_WITH_STRICT_TOOLCHAIN=ON，即 Ubuntu CI）→ FATAL_ERROR，
#     绝不允许"编不过就静默跳过"，否则集成证据永远拿不到却看着像绿；
#   - 本地构建（Windows/MinGW 无 libmysqlclient）→ 排除该 TU，
#     让其余单测仍可编译；此时 mysql_integration_test 找不到 TNYCL_HAVE_MYSQL，
#     main 会以退出码 1 报"阻塞"，不会伪装成 77 跳过。
if(NOT EAT_MYSQL_FOUND)
  if(BUILD_WITH_STRICT_TOOLCHAIN)
    message(FATAL_ERROR
      "验收构建要求 libmysqlclient（Ubuntu: apt install libmysqlclient-dev）。"
      "缺库时 src/adapters/mysql_conn.cpp 无法编译，真实 MySQL 集成证据无从产生。")
  endif()
  list(REMOVE_ITEM EAT_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/mysql_conn.cpp)
  message(WARNING
    "未找到 libmysqlclient：本次构建排除 mysql_conn.cpp（该构建不具备真实 MySQL 集成能力）")
endif()

if(EAT_CORE_SOURCES)
  add_library(tnycl_core STATIC ${EAT_CORE_SOURCES})
else()
  # 允许"只提交了骨架"的中间态也能配置成功（CI 分阶段绿，而不是一开始就红）
  add_library(tnycl_core STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/empty_tu.cpp)
  message(WARNING "tnycl_core 暂无实现源文件，使用占位 TU")
endif()
target_include_directories(tnycl_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# nlohmann_json 必须**显式链接**：系统包装在 /usr/include 时靠默认搜索路径"恰好能用"，
# 但走 FetchContent 兜底时头文件在 build/_deps/ 下，不链接就会
# "nlohmann/json.hpp: No such file or directory"。这里用 PUBLIC，
# 让所有依赖 tnycl_core 的可执行体（main/各测试）自动获得 include 路径。
target_link_libraries(tnycl_core
  PUBLIC Threads::Threads nlohmann_json::nlohmann_json
  PRIVATE eat_warnings)

# ============================================================================
# libmysqlclient 链接（T-2.04）
# TNYCL_HAVE_MYSQL 用 PUBLIC：mysql_integration_test 需要它来决定是否接线真实连接池
# （#ifdef 在测试侧，而非把 <mysql.h> 泄漏进测试 TU —— D-04 只允许 adapters 层碰 C 头）。
# PRIVATE 链接：静态库的 PRIVATE 依赖仍会以 LINK_ONLY 传给最终可执行体，
# 保证测试二进制能解析 mysql_* 符号，同时不污染其他目标的编译接口。
# ============================================================================
if(EAT_MYSQL_FOUND)
  target_compile_definitions(tnycl_core PUBLIC TNYCL_HAVE_MYSQL=1)
  if(EAT_MYSQL_LINK)
    target_link_libraries(tnycl_core PRIVATE ${EAT_MYSQL_LINK})
  else()
    target_include_directories(tnycl_core PRIVATE ${MYSQL_INCLUDE_DIR})
    target_link_libraries(tnycl_core PRIVATE ${MYSQL_LIBRARY})
  endif()
endif()

# ============================================================================
# libargon2：T-3.02 密码哈希的生产原语（cmake/FindArgon2.cmake 由 Agent B 提供，
# 按该文件注释「由协调者在 CMakeLists.txt 中接线」在此接线）。
#
# 为什么必须在验收构建里硬失败：无库时 argon2_hasher.cpp 会走 stub 分支——
# 编译能过、单元测试可能全绿，但 hash/verify 一调用就抛异常。
# 这种"CI 绿、生产崩"的组合比直接编不过更危险，所以 BUILD_WITH_STRICT_TOOLCHAIN=ON
# （验收构建）时缺库直接 FATAL_ERROR；本地开发构建才允许走 stub。
# ============================================================================
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(Argon2 QUIET)
if(TARGET Argon2::Argon2)
  message(STATUS "libargon2: 已找到，启用真实 Argon2id 实现")
  target_compile_definitions(tnycl_core PRIVATE TNYCL_HAVE_ARGON2=1)
  target_link_libraries(tnycl_core PRIVATE Argon2::Argon2)
elseif(BUILD_WITH_STRICT_TOOLCHAIN)
  message(FATAL_ERROR
    "验收构建要求 libargon2（Ubuntu: apt install libargon2-dev）。"
    "缺库时 hash/verify 会在运行时抛异常，不允许以 stub 通过验收。")
else()
  message(WARNING
    "未找到 libargon2：本次构建使用 stub（hash/verify 会抛异常），"
    "仅用于本地编译检查。验收必须安装 libargon2-dev。")
endif()


# ============================================================================
# 可执行体（T-1.01 骨架）
# ============================================================================
add_executable(eat_server src/main.cpp)
target_link_libraries(eat_server PRIVATE tnycl_core eat_warnings)

# ============================================================================
# 测试：tests/<模块>/<模块>_test.cpp 每个文件一个可执行体（各自带 main）
# 自动发现——新增模块测试不需要改本文件（W1–W8 各 Agent 并行加测试）
# ============================================================================
enable_testing()

file(GLOB EAT_TEST_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/*/*_test.cpp)

foreach(test_src ${EAT_TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_include_directories(${test_name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
  )
  target_link_libraries(${test_name} PRIVATE tnycl_core eat_warnings)
  add_test(NAME ${test_name} COMMAND ${test_name})
  # 退出码 77 = "跳过"（autotools 约定）：
  # 需要真实 MySQL/Redis 的集成用例在环境不具备时返回 77，
  # ctest 应报 Skipped 而不是 Failed —— 否则没有 DB 的 CI 永远不可能绿。
  # 注意："跳过"在报告里仍然可见，不会伪装成通过。
  set_tests_properties(${test_name} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()

if(NOT EAT_TEST_SOURCES)
  message(WARNING "tests/*/*_test.cpp 未找到，本次不注册任何测试")
endif()
