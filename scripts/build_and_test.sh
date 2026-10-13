#!/usr/bin/env bash
# 一键三连：常规构建+测试 -> ASan+UBSan 构建+测试 -> TSan 构建+测试
# 退出码：任一模式构建失败或测试未全绿则非 0（验收关卡 B 依赖此退出码）。
# ASan/TSan 仅 Linux/macOS（Windows MinGW 无 libasan/libtsan，请在 WSL2 内跑取证）。
set -euo pipefail
cd "$(dirname "$0")/.."

overall=0
run_mode() {
    local mode=$1
    shift
    local dir="build/$mode"
    cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE=Release "$@"
    cmake --build "$dir" -j
    if "$dir/test_pool"; then
        echo "== $mode: 全绿 =="
    else
        echo "== $mode: 测试未全绿 ==" >&2
        overall=1
    fi
}

run_mode default

if [ "$(uname -s)" = "Linux" ] || [ "$(uname -s)" = "Darwin" ]; then
    run_mode asan -DBPOOL_SANITIZE=ON
    run_mode tsan -DBPOOL_TSAN=ON
else
    echo "== 跳过 asan/tsan（$(uname -s) 无对应运行时；取证请在 WSL2/Linux 执行）=="
fi

exit $overall
