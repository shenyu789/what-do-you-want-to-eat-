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
# 核心库：src/ 下除 main.cpp 外的全部实现
# CONFIGURE_DEPENDS 让新增源文件不必手改本文件（W1 期间各 Agent 并行加文件）
# ============================================================================
file(GLOB_RECURSE EAT_CORE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM EAT_CORE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/empty_tu.cpp)   # 占位 TU 只在无实现时才用

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
endforeach()

if(NOT EAT_TEST_SOURCES)
  message(WARNING "tests/*/*_test.cpp 未找到，本次不注册任何测试")
endif()
