#!/usr/bin/env bash
# 在 macOS 上把 iOS 宿主编成 .app 再打成 IPA。
#
# 产出默认是**未签名**的 IPA —— CI 上没有你的 Apple 账号，
# 装到设备上之前得自己重签（SideStore / AltStore / 开发者证书都行）。
#
# 用法：
#   ./platform/ios/build-app.sh --unicorn <unicorn 构建目录> --out dist/HXVM.ipa
#   ./platform/ios/build-app.sh --unicorn ... --identity "Apple Development: ..."
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

UNICORN_DIR=""
OUT="dist/HXVM.ipa"
IDENTITY=""          # 留空 = 不签名
CONFIG="Release"
DEPLOY="15.0"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --unicorn)   UNICORN_DIR="$2"; shift 2 ;;
    --out)       OUT="$2"; shift 2 ;;
    --identity)  IDENTITY="$2"; shift 2 ;;
    --config)    CONFIG="$2"; shift 2 ;;
    --deploy)    DEPLOY="$2"; shift 2 ;;
    -h|--help)   sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done

if [[ -z "$UNICORN_DIR" ]]; then
  echo "[build-app] 需要 --unicorn 指向编好的 Unicorn（含 include/ 与 libunicorn*.a）" >&2
  exit 2
fi

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
echo "[build-app] iOS SDK: $SDK"
echo "[build-app] Unicorn: $UNICORN_DIR"

BUILD="build-ios"
APPDIR="Payload/HXVM.app"

# ── 1) 核心库（含全部垫片）────────────────────────────────────────────
echo "[build-app] 编译 libhxvm.a（iOS arm64）"
cmake -G Ninja -B "$BUILD" -S . \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchains/ios.toolchain.cmake" \
  -DPLATFORM=OS64 \
  -DDEPLOYMENT_TARGET="$DEPLOY" \
  -DCMAKE_BUILD_TYPE="$CONFIG" \
  -DUNICORN_ROOT="$UNICORN_DIR" \
  -DCMAKE_OSX_SYSROOT="$SDK"
ninja -C "$BUILD" hxvm_core

LIBHXVM="$(find "$BUILD" -name 'libhxvm*.a' -print -quit)"
if [[ -z "$LIBHXVM" ]]; then
  echo "[build-app] libhxvm.a 没编出来" >&2
  exit 1
fi
echo "[build-app] 核心库：$LIBHXVM"

# ── 2) 宿主 Objective-C++ ─────────────────────────────────────────────
rm -rf "$APPDIR"
mkdir -p "$APPDIR"

XCRUN=(xcrun -sdk iphoneos clang++ -arch arm64 -miphoneos-version-min="$DEPLOY")
COMMON_FLAGS=(-std=c++17 -fobjc-arc -fno-exceptions -fno-rtti -O2
              -isysroot "$SDK"
              -I "$ROOT/core/include"
              -I "$ROOT"
              -I "$UNICORN_DIR/include")

echo "[build-app] 编译 main.mm"
"${XCRUN[@]}" "${COMMON_FLAGS[@]}" -c platform/ios/main.mm -o "$BUILD/main.mm.o"

# ── 3) 链接 ───────────────────────────────────────────────────────────
echo "[build-app] 链接 HXVM"
"${XCRUN[@]}" "$BUILD/main.mm.o" -o "$APPDIR/HXVM" \
  "$LIBHXVM" \
  "$UNICORN_DIR/lib/libunicorn.a" \
  -lc++ \
  -framework UIKit -framework Foundation -framework QuartzCore \
  -framework Metal -framework CoreGraphics \
  -framework AudioToolbox -framework AVFoundation

# ── 4) 资源 ───────────────────────────────────────────────────────────
cp platform/ios/Info.plist "$APPDIR/Info.plist"

# 如果本地放了 MobileVLCKit（音频/视频走它），一起嵌进去
if [[ -d "third_party/MobileVLCKit.framework" ]]; then
  echo "[build-app] 嵌入 MobileVLCKit"
  mkdir -p "$APPDIR/Frameworks"
  cp -R third_party/MobileVLCKit.framework "$APPDIR/Frameworks/"
fi

# ── 5) 签名（可选）────────────────────────────────────────────────────
if [[ -n "$IDENTITY" ]]; then
  echo "[build-app] 用身份签名：$IDENTITY"
  if [[ -d "$APPDIR/Frameworks/MobileVLCKit.framework" ]]; then
    codesign --force --sign "$IDENTITY" \
      --entitlements platform/ios/Entitlements.plist \
      "$APPDIR/Frameworks/MobileVLCKit.framework"
  fi
  codesign --force --sign "$IDENTITY" \
    --entitlements platform/ios/Entitlements.plist "$APPDIR"
else
  echo "[build-app] 未签名（CI 上没有 Apple 账号，装设备前需自签）"
fi

# ── 6) 打 IPA ─────────────────────────────────────────────────────────
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
( cd "$(dirname "$APPDIR")" && zip -qry "$(cd - >/dev/null; cd "$ROOT" && pwd)/$OUT" Payload )
echo "[build-app] 产出：$OUT"
ls -la "$OUT"
