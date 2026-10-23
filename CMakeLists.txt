cmake_minimum_required(VERSION 3.16)
project(iOS-HaxeExec-VM LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ── Unicorn ────────────────────────────────────────────────────────────
# 来源优先级：UNICORN_ROOT（scripts/fetch-unicorn.sh 拉下来的）> pip 装的 > 系统
# 交叉编译 iOS 时 toolchain 会把 find_* 的根钉在 iOS SDK 上（不然 CMake 会在
# macOS 宿主路径找到 IOKit 这类桌面框架），但 Unicorn 是第三方依赖，不在 SDK 里。
# find_path / find_library 在这套约束下的行为很难预测（NO_CMAKE_FIND_ROOT_PATH
# 也不总能救回来），所以这里直接用显式路径判断，行为可预测。
if(UNICORN_ROOT)
  if(EXISTS "${UNICORN_ROOT}/include/unicorn/unicorn.h")
    set(UNICORN_INCLUDE_DIR "${UNICORN_ROOT}/include")
  endif()
  # Unicorn 2.x 产出 libunicorn-static.a，v1 才是 libunicorn.a —— 两个都认
  file(GLOB _hxvm_uc_libs "${UNICORN_ROOT}/lib/libunicorn*.a")
  if(_hxvm_uc_libs)
    list(GET _hxvm_uc_libs 0 UNICORN_LIBRARY)
  endif()
endif()

# 没给 UNICORN_ROOT 时（开发机上用 pip 装的）才回落到查找
if(NOT UNICORN_INCLUDE_DIR OR NOT EXISTS "${UNICORN_INCLUDE_DIR}/unicorn/unicorn.h")
  find_path(UNICORN_INCLUDE_DIR
    NAMES unicorn/unicorn.h
    HINTS
      /usr/local/lib/python3.12/dist-packages/unicorn/include
      /usr/local/include
      /usr/include
  )
endif()
if(NOT UNICORN_LIBRARY)
  find_library(UNICORN_LIBRARY
    NAMES unicorn unicorn-static
    HINTS
      /usr/local/lib/python3.12/dist-packages/unicorn/lib
      /usr/local/lib
      /usr/lib
  )
endif()

if(NOT UNICORN_INCLUDE_DIR OR NOT UNICORN_LIBRARY)
  message(FATAL_ERROR
    "找不到 Unicorn。先跑 ./scripts/fetch-unicorn.sh 再用 -DUNICORN_ROOT=... 指过来。")
endif()
message(STATUS "Unicorn 头文件: ${UNICORN_INCLUDE_DIR}")
message(STATUS "Unicorn 库　　: ${UNICORN_LIBRARY}")

# ── 核心库 ─────────────────────────────────────────────────────────────
set(HXVM_CORE_SOURCES
  core/src/memory.cpp
  core/src/filesystem.cpp
  core/src/registry.cpp
  core/src/shim.cpp
  core/src/cpu_unicorn.cpp
  core/src/loader.cpp
  core/src/process.cpp
  shims/kernel32.cpp
  shims/crt.cpp
  shims/user32.cpp
  shims/advapi32.cpp
  shims/vlc.cpp
  shims/sdl2.cpp
  shims/media_backend.cpp
)

add_library(hxvm_core STATIC ${HXVM_CORE_SOURCES})
target_include_directories(hxvm_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core/include
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${UNICORN_INCLUDE_DIR}
)
target_compile_options(hxvm_core PRIVATE -Wall -Wextra)

# ── 命令行宿主（开发机/iOS 之外都用得上）────────────────────────────
add_executable(hxvm-cli host/cli.cpp)
target_link_libraries(hxvm-cli PRIVATE hxvm_core ${UNICORN_LIBRARY} pthread m)
