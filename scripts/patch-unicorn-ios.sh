#!/usr/bin/env bash
# 让 Unicorn 能交叉编译到 iOS（真机 arm64）。
#
# 障碍：iOS SDK 把 pthread_jit_write_protect_np 声明成 unavailable ——
#   这个函数需要 com.apple.security.cs.allow-jit 权限，Apple 不想让普通 App 用，
#   所以在公开头里直接标了不可用。编译期一旦"使用"它就报 error，不是 warning。
# 但这个符号在 iOS 的 libSystem 里真实存在，拿到 JIT 权限就能正常调用
# （这也正是本项目得以存在的前提）。
#
# 解法：不碰 SDK 头，只把 Unicorn 源码里的调用全部改指向一个 asm 别名，
#   别名指向同一个符号 _pthread_jit_write_protect_np。
#   这样 SDK 里那条 unavailable 声明就没人再"使用"，可用性检查自然不触发。
#
# 注意：macOS 自带的 bash 是 3.2（因为许可证问题 Apple 不升级），
#   没有 mapfile / readarray，别用。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ ! -d unicorn ]; then
  echo "[patch-unicorn] 先跑 ./scripts/fetch-unicorn.sh" >&2
  exit 1
fi

ALIAS='extern int hxvm_jit_write_protect_np(int) __asm("_pthread_jit_write_protect_np");'

# BSD grep 不保证支持 --include，所以用 find + xargs
FILES=$(
  find unicorn -type f \( -name '*.c' -o -name '*.h' \) -print0 2>/dev/null \
    | xargs -0 grep -l 'pthread_jit_write_protect_np' 2>/dev/null || true
)

if [ -z "$FILES" ]; then
  echo "[patch-unicorn] 没有需要修补的文件（该版本可能不依赖这个 API）"
  exit 0
fi

COUNT=0
printf '%s\n' "$FILES" | while IFS= read -r f; do
  [ -z "$f" ] && continue
  # 已经补过就跳过，保证可重复执行
  if grep -q 'hxvm_jit_write_protect_np' "$f"; then
    echo "[patch-unicorn] 跳过（已修补）：$f"
    continue
  fi

  # 顺序很关键：先改名（此时别名声明还没写进去，不会被误改），再插入别名声明。
  # 反过来会把别名里的 "_pthread_jit_write_protect_np" 一起改坏。
  python3 - "$f" "$ALIAS" <<'PY'
import sys
path, alias = sys.argv[1], sys.argv[2]
with open(path, encoding="utf-8", errors="replace") as fh:
    src = fh.read()
src = src.replace("pthread_jit_write_protect_np", "hxvm_jit_write_protect_np")
src = alias + "\n" + src
with open(path, "w", encoding="utf-8") as fh:
    fh.write(src)
PY
  echo "[patch-unicorn] 已修补：$f"
  COUNT=$((COUNT + 1))
  echo "$COUNT" > .patch-count
done

if [ -f .patch-count ]; then
  echo "[patch-unicorn] 完成，本次修补 $(cat .patch-count) 个文件"
  rm -f .patch-count
fi
