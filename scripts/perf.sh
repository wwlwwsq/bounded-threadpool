#!/usr/bin/env bash
# 性能调优取证：热点定位 -> 改动 -> 前后对比，所有证据在这里产出
# 前置（WSL2/Ubuntu）: sudo apt install linux-tools-generic
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=build/default/bench_pool
if [ ! -x "$BIN" ]; then
    echo "先跑 scripts/build_and_test.sh 生成 build/default"
    exit 1
fi

echo "== perf stat（调度与上下文切换概况）=="
perf stat -e task-clock,context-switches,cpu-migrations "$BIN" 200000 || true

echo
echo "== perf record（热点函数，含调用栈）=="
perf record -g -F 999 -o perf.data "$BIN" 200000 || true
perf report -i perf.data --stdio 2>/dev/null | head -40 || true
echo
echo "交互式查看: perf report -i perf.data    （-g 展开调用栈）"
echo "火焰图(可选): git clone https://github.com/brendangregg/FlameGraph &&"
echo "  perf script -i perf.data | FlameGraph/stackcollapse-perf.pl | FlameGraph/flamegraph.pl > pool.svg"
