# cmake/FindArgon2.cmake —— libargon2 检测（T-3.02 生产链接）
#
# 用法（由协调者在 CMakeLists.txt 中接线，Agent A 不擅自改共享 CMakeLists）：
#   find_package(Argon2 REQUIRED)
#   if(TARGET Argon2::Argon2)
#     target_compile_definitions(tnycl_core PRIVATE TNYCL_HAVE_ARGON2=1)
#     target_link_libraries(tnycl_core PRIVATE Argon2::Argon2)
#   endif()
#
# 检测：优先 pkg-config（libargon2.pc），回退到 find_library + find_path。
# 不写 URL_HASH / 固定版本：版本以系统包为准，避免编造哈希。

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_ARGON2 QUIET argon2)
endif()

find_path(ARGON2_INCLUDE_DIR NAMES argon2.h HINTS ${PC_ARGON2_INCLUDE_DIRS})
find_library(ARGON2_LIBRARY NAMES argon2 HINTS ${PC_ARGON2_LIBRARY_DIRS})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Argon2
  REQUIRED_VARS ARGON2_LIBRARY ARGON2_INCLUDE_DIR)

if(Argon2_FOUND AND NOT TARGET Argon2::Argon2)
  add_library(Argon2::Argon2 UNKNOWN IMPORTED)
  set_target_properties(Argon2::Argon2 PROPERTIES
    IMPORTED_LOCATION "${ARGON2_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ARGON2_INCLUDE_DIR}")
endif()

mark_as_advanced(ARGON2_INCLUDE_DIR ARGON2_LIBRARY)
