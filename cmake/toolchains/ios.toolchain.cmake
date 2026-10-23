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
  set(_hxvm_sdk "iphonesimulator")
else()
  set(_hxvm_sdk "iphoneos")
endif()
set(CMAKE_OSX_SYSROOT "${_hxvm_sdk}")

# 拿 SDK 的绝对路径，用它当 find_library / find_path 的根。
#
# 不这么做的话，CMake 会在 macOS 的宿主路径里找到 IOKit、CoreFoundation 这类
# 桌面框架（iOS SDK 里根本没有 IOKit），然后把 -framework IOKit 塞进链接参数，
# 交叉编译必然失败。Unicorn 就是踩在这个点上。
execute_process(
  COMMAND xcrun --sdk ${_hxvm_sdk} --show-sdk-path
  OUTPUT_VARIABLE HXVM_IOS_SDK
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
set(CMAKE_FIND_ROOT_PATH "${HXVM_IOS_SDK}")

set(CMAKE_C_FLAGS   "-miphoneos-version-min=${DEPLOYMENT_TARGET}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "-miphoneos-version-min=${DEPLOYMENT_TARGET} -fno-exceptions -fno-rtti" CACHE STRING "" FORCE)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 静态库，别去链接 App Store 不允许的动态库
set(CMAKE_MACOSX_BUNDLE NO)
