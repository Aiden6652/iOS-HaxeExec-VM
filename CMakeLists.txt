cmake_minimum_required(VERSION 3.16)
project(iOS-HaxeExec-VM LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ── Unicorn ────────────────────────────────────────────────────────────
# 来源优先级：UNICORN_ROOT（scripts/fetch-unicorn.sh 拉下来的）> pip 装的 > 系统
find_path(UNICORN_INCLUDE_DIR
  NAMES unicorn/unicorn.h
  HINTS
    ${UNICORN_ROOT}/include
    /usr/local/lib/python3.12/dist-packages/unicorn/include
    /usr/local/include
    /usr/include
)
find_library(UNICORN_LIBRARY
  NAMES unicorn
  HINTS
    ${UNICORN_ROOT}/lib
    /usr/local/lib/python3.12/dist-packages/unicorn/lib
    /usr/local/lib
    /usr/lib
)

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
