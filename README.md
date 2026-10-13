# bounded-threadpool

C++17 有界队列线程池：单锁基线版 `ThreadPool` 与分片优化版 `ShardedThreadPool`。
围绕 JD 关键词「高并发 / 稳定性 / 性能调优」构建证据链：规格测试 + TSan/ASan + perf 热点 + 前后对比数据。

## 构建

```bash
cmake -S . -B build && cmake --build build -j
./build/test_pool            # 规格测试（骨架阶段预期全 FAILED: not implemented）
./build/bench_pool 1000000   # 吞吐基准，输出 markdown 表行
bash scripts/build_and_test.sh   # 常规 + ASan + TSan 三连（Linux/WSL2）
```

## 规格 ↔ 测试对照

| 规格条目 | 测试 |
|---|---|
| future 返回值 | test_future_return_value |
| 异常经 future 传递、worker 存活 | test_exception_propagates_and_pool_survives |
| kThrow 拒绝 + 计数 | test_throw_policy_rejects_when_full |
| kBlock 背压 | test_block_policy_backpressure |
| kCallerRuns 在调用方执行 | test_caller_runs_executes_on_caller |
| shutdown(true) 排空不丢任务 | test_shutdown_drains_all_tasks |
| 关闭后提交报错 | test_submit_after_shutdown_throws |
| stats 计数正确 | test_stats_counters |
| 并发提交压力（TSan 重点） | test_concurrent_submit_stress |
| 分片版功能等价 | test_sharded_pool_equivalent |

## TODO 实现顺序（grep "TODO(" 看全部）

1. TODO(1)–(2)：`include/bpool/thread_pool.hpp` 的 post / submit 模板
2. TODO(3)–(6)：`src/thread_pool.cpp` 生命周期与 worker 循环 → 跑 `test_pool` 至全绿
3. TODO(7)–(8)：`include/bpool/sharded_pool.hpp` 模板
4. TODO(9)–(13)：`src/sharded_pool.cpp` 分片实现 → 跑 bench 对比

## 性能数据（边做边填，全部实测）

### 吞吐：workers 扫描（任务 = atomic 自增，100 万次）

| pool | workers | queue_cap(shard) | M ops/s |
|---|---|---|---|
| baseline | 1 | 4096 | |
| baseline | 4 | 4096 | |
| baseline | 8 | 4096 | |
| sharded | 8 | 256 | |

### 优化记录（每行 = 一次「假设 -> 改动 -> 数据」）

| 日期 | 假设 | 改动 | 数据（前 -> 后） | 工具 |
|---|---|---|---|---|
| | 全局单锁是瓶颈 | 单队列 -> 分片队列 | 8 线程 __ M -> __ M ops/s | perf record |

## gdb 取证：制造并定位一次死锁（面试弹药）

```bash
cat > /tmp/deadlock.cpp <<'EOF'
#include <chrono>
#include <mutex>
#include <thread>
int main() {
    std::mutex a, b;
    std::thread t([&] {
        std::lock_guard<std::mutex> x(a);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard<std::mutex> y(b);
    });
    std::lock_guard<std::mutex> y(b);
    std::lock_guard<std::mutex> x(a);
    t.join();
}
EOF
g++ -g -O0 /tmp/deadlock.cpp -o /tmp/deadlock -pthread && /tmp/deadlock
# 卡住后另开终端:
gdb -p $(pidof deadlock)
(gdb) thread apply all bt    # 两个线程互相持锁等待 —— 这就是「熟练 gdb」的故事素材
```

## 简历 bullet（数据跑出来再填，别提前写）

- 实现有界队列线程池：双条件变量同步、future 异步返回、异常透传、可配置拒绝策略与优雅关闭；100 万任务 8 线程吞吐 __M ops/s
- 以单锁版为基线，用 perf 定位锁竞争热点后实现分片队列版本，8 线程吞吐提升 __%；TSan/ASan 全量通过
