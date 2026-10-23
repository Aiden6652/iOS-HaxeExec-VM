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
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ ! -d unicorn ]; then
  echo "[patch-unicorn] 先跑 ./scripts/fetch-unicorn.sh" >&2
  exit 1
fi

ALIAS='extern int hxvm_jit_write_protect_np(int) __asm("_pthread_jit_write_protect_np");'

# 只处理 Unicorn 自己的源码（SDK 的 pthread.h 绝不能动）
mapfile -t FILES < <(grep -rl "pthread_jit_write_protect_np" unicorn \
                       --include="*.c" --include="*.h" 2>/dev/null || true)

if [ ${#FILES[@]} -eq 0 ]; then
  echo "[patch-unicorn] 没有需要修补的文件（该 Unicorn 版本可能不依赖这个 API）"
  exit 0
fi

for f in "${FILES[@]}"; do
  # 已经补过就跳过，保证可重复执行
  if grep -q "hxvm_jit_write_protect_np" "$f"; then
    echo "[patch-unicorn] 跳过（已修补）：$f"
    continue
  fi

  # 顺序很重要：先改名（此时别名声明还没写进去，不会被误改），再插入别名声明
  python3 - "$f" "$ALIAS" <<'PY'
import sys
path, alias = sys.argv[1], sys.argv[2]
src = open(path, encoding="utf-8", errors="replace").read()
src = src.replace("pthread_jit_write_protect_np", "hxvm_jit_write_protect_np")
# 别名声明放在最前面，保证在任何使用之前
src = alias + "\n" + src
open(path, "w", encoding="utf-8").write(src)
PY
  echo "[patch-unicorn] 已修补：$f"
done

echo "[patch-unicorn] 完成，共 ${#FILES[@]} 个文件"
