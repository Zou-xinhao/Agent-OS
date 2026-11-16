#!/usr/bin/env bash
# 三人三台机器版本对齐检查：谁的版本不一样，谁重建环境
set -u
fail=0

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "[MISS] $1 未安装"
    fail=1
  fi
}

need riscv64-linux-gnu-gcc
need qemu-system-riscv64
need gdb-multiarch
need make
need git

echo "--- 版本（必须三人一致，差异写进 README）---"
riscv64-linux-gnu-gcc --version 2>/dev/null | head -1
qemu-system-riscv64 --version    2>/dev/null | head -1
gdb-multiarch --version 2>/dev/null | head -1
make --version 2>/dev/null | head -1
git --version 2>/dev/null | head -1

echo "--- 工具前缀自检 ---"
if grep -q 'riscv64-unknown-elf-' Makefile 2>/dev/null; then
  echo "[WARN] Makefile 仍是 riscv64-unknown-elf-，本机没有该工具链会编译失败"
  echo "       修复：sed -i 's/riscv64-unknown-elf-/riscv64-linux-gnu-/' Makefile"
  fail=1
fi

if [ "$fail" -eq 0 ]; then
  echo "[OK] 环境检查通过"
else
  echo "[FAIL] 环境不完整，按上面的提示修"
fi
exit "$fail"
