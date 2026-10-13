# bounded-threadpool

C++17 有界队列线程池：单锁基线版 `ThreadPool` 与分片优化版 `ShardedThreadPool`。
围绕 JD 关键词「高并发 / 稳定性 / 性能调优」构建证据链：规格测试 + TSan/ASan + perf 热点 + 前后对比数据。

> **[开发计划与验收标准](docs/DEVELOPMENT_PLAN.md)** —— 里程碑 M0–M5、五道验收关卡（功能/正确性/性能/文档/提交卫生）、设计决策点 D1–D7、风险预案与面试讲解映射。当前进度见该文档 §1/§2 与文末变更记录。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/test_pool            # 规格测试（11 条用例，全绿退出码 0）
./build/stress_mix           # 混合压力：并发 post/submit × 三策略 × 并发 shutdown，计数守恒
./build/bench_pool 1000000 5 # 吞吐基准（任务数 reps），输出 markdown 表行（取中位）
bash scripts/build_and_test.sh   # 常规 + ASan + TSan 三连（Linux/WSL2）
```

无 cmake 的 Windows/MinGW 机器上的等价验证（本机 i686 g++ 15.2 实测可用）：

```bash
# 功能验证
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread -Iinclude src/*.cpp tests/test_pool.cpp -o test_pool && ./test_pool
# 本机可用的 UB 检查（MinGW 无 libasan/libubsan，trap 模式不依赖运行时库，有 UB 即 SIGILL）
g++ -std=c++17 -O1 -g -fsanitize=undefined -fsanitize-undefined-trap-on-error -pthread -Iinclude src/*.cpp tests/test_pool.cpp -o test_pool.ubsan && ./test_pool.ubsan
```

## 规格 ↔ 测试对照

| 规格条目 | 测试 |
|---|---|
| future 返回值 | test_future_return_value |
| 异常经 future 传递、worker 存活 | test_exception_propagates_and_pool_survives |
| kThrow 拒绝 + 计数 | test_throw_policy_rejects_when_full |
| kBlock 背压 | test_block_policy_backpressure |
| kCallerRuns 在调用方执行 | test_caller_runs_executes_on_caller |
| shutdown(true) 排空不丢任务（返回即完成） | test_shutdown_drains_all_tasks |
| 关闭后提交报错 | test_submit_after_shutdown_throws |
| stats 计数正确 | test_stats_counters |
| 并发提交压力（TSan 重点） | test_concurrent_submit_stress |
| 分片版功能等价 | test_sharded_pool_equivalent |
| shutdown(false) 丢弃任务 → future 立即 broken_promise | test_shutdown_discard_breaks_futures |

## 设计决策（D1–D7，面试讲解弹药）

- **D1 关闭时阻塞中的提交者**：`kBlock` 的 post 在 `closed_` 置位后被唤醒，必须抛 `PoolClosedError`，任务**不入队不执行**。丢任务是正确行为：若放它入队，`shutdown(true)`「返回即全部完成」的契约要么被破坏、要么永远等不到（提交者可能仍卡在满队列上）。任务还握在提交者手里，用异常通知它自行决定重试或放弃，比悄悄排队更可控。
- **D2 caller-runs 计数语义**（已定稿）：同步执行的任务 `submitted++` **且** `completed++`——`completed` 的语义是「执行完毕的任务数」，与执行线程无关；stats 不因线程不同而双标（test_stats_counters 不涉及 caller-runs，断言自洽）。
- **D3 worker 在 pop 之后（而非执行完之后）notify_not_full**：空位在任务执行前就对生产者可见，任务执行期间生产者可并行入队，吞吐关键路径更短。**每次 pop 都 notify_one，与让出的空位一一配对**——曾实现「仅当队列原本满时才通知」想省一次系统调用，被 stress_mix 首轮捕获死锁：多个提交者在满队时堆积睡眠、队列随后被连续消费排空时，只有第一个拿到通知，其余提交者谓词已真（有空位）却无人唤醒，永久睡眠。教训：谓词等待只能容忍**多余**通知，救不了**缺失**的通知。
- **D4 分片不偷任务**：worker 只消费绑定 shard，队头倾斜时负载不均。轮转选片在任务粒度均匀时倾斜有限（见数据表）；倾斜敏感场景的出路是「选最浅片」或 work-stealing（§6 加分项，用数据说话）。
- **D5 shutdown(false) 丢弃任务的 future 状态**（修正后定稿）：实现必须在 `shutdown(false)` 内**就地清空队列**——被丢弃任务里的 `packaged_task` 随之销毁，对应 future 立即以 `std::future_error(broken_promise)` 就绪。若不清队，future 要等到池析构才打破，期间 `get()` 永久阻塞（原方案描述的行为与骨架代码不符，已修正并用 test_shutdown_discard_breaks_futures 机器验证）。
- **D6 每个 shard 至少一个 worker**：`num_shards > num_workers` 时，无消费者的 shard 任务永远不被执行、drain 卡死。构造时 worker 数取 `max(num_workers, num_shards)`。
- **D7 shutdown 同步语义**：`drain=true` 阻塞到「已入队任务全部完成、worker 全部退出」才返回（`cv_shutdown_` + `active_workers_` 计数）；`drain=false` 原子清队后立即返回（运行中任务无法安全打断，由析构 join 收尾）。约束：**不得在任务体内调用 shutdown(true)**——等价于自己等自己，死锁。构造函数在线程创建中途失败时同样关闸回收已启动线程，不让线程逃过析构。

## 性能数据（全部实测；取证环境以 WSL2/x86-64 为准）

### 吞吐：workers 扫描（任务 = atomic 自增，100 万次，M4 待 WSL2 实测填入）

| pool | workers | queue_cap(shard) | M ops/s |
|---|---|---|---|
| baseline | 1 | 4096 | 待测 |
| baseline | 2 | 4096 | 待测 |
| baseline | 4 | 4096 | 待测 |
| baseline | 8 | 4096 | 待测 |
| sharded | 1 | 256 | 待测 |
| sharded | 2 | 256 | 待测 |
| sharded | 4 | 256 | 待测 |
| sharded | 8 | 256 | 待测 |

> 本机（Windows / 32 位 i686 MinGW g++ 15.2 / 16 逻辑核，后台负载高）单次运行参考数据：
> baseline 1/2/4/8/16 workers ≈ 6.5 / 6.1 / 3.4 / 0.2 / 0.04 M ops/s；sharded ≈ 5.7 / 1.7 / 0.3 / 0.2 / 0.2 M ops/s。
> 该环境**不满足**关卡 C 的 1 线程 sanity check（差距 12–30%），且高 worker 数全线坍塌、run-to-run 方差巨大。
> 归因假设：i686 上 `atomic<uint64_t>` 无原生支持（每次计数走 lock cmpxchg8b，且跨线程争用）+ winpthreads 条件变量开销；
> 待 WSL2 x86-64（原生 64 位 lock add）复测后再下结论——这正是「有数据的负结论也算过」的场景，但在噪声环境里连负结论都不可信。

### 优化记录（每行 = 一次「假设 -> 改动 -> 数据」；M4 在 WSL2 上闭环 ≥2 条）

| 日期 | 假设 | 改动 | 数据（前 -> 后） | 工具 |
|---|---|---|---|---|
| 待 M4 | 全局单锁是瓶颈 | 单队列 -> 分片队列 | 待 perf 定位后填 | perf record |

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

- 实现有界队列线程池：双条件变量同步、future 异步返回、异常透传、可配置拒绝策略与优雅关闭；100 万任务 8 线程吞吐待测 M ops/s
- 以单锁版为基线，用 perf 定位锁竞争热点后实现分片队列版本，8 线程吞吐提升待测%；TSan/ASan 全量通过
