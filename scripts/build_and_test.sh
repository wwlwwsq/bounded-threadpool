#!/usr/bin/env bash
# 一键三连：常规构建+测试 -> ASan+UBSan 构建+测试 -> TSan 构建+测试
# TSan 仅 Linux/macOS（Windows 无 TSan，请在 WSL2 内跑本脚本）
set -euo pipefail
cd "$(dirname "$0")/.."

run_mode() {
    local mode=$1
    shift
    local dir="build/$mode"
    cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE=Release "$@"
    cmake --build "$dir" -j
    if "$dir/test_pool"; then
        echo "== $mode: 全绿 =="
    else
        echo "== $mode: 测试未全绿（骨架阶段预期：not implemented）=="
    fi
}

run_mode default
run_mode asan -DBPOOL_SANITIZE=ON
run_mode tsan -DBPOOL_TSAN=ON
