#!/usr/bin/env bash
# 拉取 Unicorn 源码到本地 unicorn/ 目录（CI 与本机通用）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UNICORN_VERSION="${UNICORN_VERSION:-2.1.4}"
DEST="$ROOT/unicorn"

if [ -d "$DEST/.git" ]; then
  echo "[fetch-unicorn] 已存在，跳过克隆"
else
  rm -rf "$DEST"
  git clone --depth 1 --branch "$UNICORN_VERSION" \
    https://github.com/unicorn-engine/unicorn.git "$DEST"
  pushd "$DEST" >/dev/null
  git submodule update --init --recursive --depth 1
  popd >/dev/null
fi

echo "[fetch-unicorn] 就绪：$DEST ($UNICORN_VERSION)"
