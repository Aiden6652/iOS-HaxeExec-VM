# 交叉编译到 iOS（真机 arm64）的极简 CMake toolchain
# 用法：
#   cmake -G Ninja -B build-ios \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.toolchain.cmake \
#     -DPLATFORM=OS64
# PLATFORM: OS64（iPhone 真机）| SIMULATORARM64（Apple Silicon 模拟器）

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)

set(PLATFORM "OS64" CACHE STRING "OS64 | SIMULATORARM64")
set(DEPLOYMENT_TARGET "15.0" CACHE STRING "最低 iOS 版本")

set(CMAKE_OSX_ARCHITECTURES arm64)

if(PLATFORM STREQUAL "SIMULATORARM64")
  set(CMAKE_OSX_SYSROOT "iphonesimulator")
else()
  set(CMAKE_OSX_SYSROOT "iphoneos")
endif()

set(CMAKE_C_FLAGS   "-miphoneos-version-min=${DEPLOYMENT_TARGET}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "-miphoneos-version-min=${DEPLOYMENT_TARGET} -fno-exceptions -fno-rtti" CACHE STRING "" FORCE)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 静态库，别去链接 App Store 不允许的动态库
set(CMAKE_MACOSX_BUNDLE NO)
