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

## 性能数据（实测环境：WSL2 Docker 容器 ubuntu:24.04，g++ 13.3 -O2，x86-64，16 vCPU，宿主机高负载）

### 吞吐：workers 扫描（任务 = atomic 自增，100 万次，reps=5 取中位）

| pool | workers | queue_cap(shard) | M ops/s (median of 5) |
|---|---|---|---|
| baseline | 1 | 4096 | 6.01 |
| sharded | 1 | 256 | 4.71 |
| baseline | 2 | 4096 | 6.74 |
| sharded | 2 | 256 | 4.88 |
| baseline | 4 | 4096 | 2.06 |
| sharded | 4 | 256 | 0.17 |
| baseline | 8 | 4096 | 0.66 |
| sharded | 8 | 256 | 0.09 |
| baseline | 16 | 4096 | 0.10 |
| sharded | 16 | 256 | 0.09 |

### 归因分析（为什么本环境分片无收益、高 worker 数坍塌——全部有数据）

1. **纯路径两池持平**：200 任务（队列装得下、kBlock 零阻塞）×1001 次取中位：
   baseline 3.60 vs sharded 3.56 M ops/s（差 1.2%，关卡 C 的 5% sanity check 在同口径下达标）。
   分片结构本身（rr_ 原子选片、锁切换）不是瓶颈。
2. **小容量 + kBlock 的唤醒税**：满载稳态下每 pop 一次唤醒一个睡眠中的提交者（futex 往返）。
   sharded 容量 256→4096 后 1 worker 差距从 22% 收窄到 9%（实测 4.71→5.77）。
3. **分片流量摊薄放大 worker 睡眠**：轮转选片使每片只分到 1/N 流量，片级队列易空、
   其绑定 worker 睡得更频繁；baseline 全部流量集中一个队列，worker 反而不睡。
4. **环境 CPU 饥饿**：perf stat 实测 task-clock 仅 **1.75 CPU / 16 vCPU**（宿主机高负载），
   每次唤醒都要等空闲 CPU，睡眠/唤醒代价被放大一个数量级；4/8/16 worker 档的坍塌
   （6.7M → 0.1M）主要由该因素驱动。
5. **perf record 热点佐证**（200k 任务，-F 299 采样）：`pthread_cond_wait` 24.7% +
   `pthread_cond_signal` 16.6% + `__lll_lock_wake` 15.2% + 内核侧 futex 路径（unknown 内核符号）
   约 35%——**时间几乎全花在同步原语上**，任务体（atomic 自增）占比可忽略。
   结论：本环境的负结果成立且有归因；在安静、CPU 富余的机器上分片预期才有正收益，
   复测命令即 `./build/bench_pool 1000000 5`。

### 优化记录（假设 -> 改动 -> 前后数据 -> 工具）

| # | 日期 | 假设 | 改动 | 数据（前 -> 后） | 工具 | 结论 |
|---|---|---|---|---|---|---|
| 1 | 2026-10-13 | 仅队列满时 notify_not_full 可省系统调用 | worker pop 后条件化 notify_one | stress_mix 首轮（kBlock cap=4）**死锁**；A/B 吞吐：条件版仅快 0–12%（1w 档），≥4w 持平（6.14→6.88 / 6.00→6.49 / 2.30≈2.32 M） | stress_mix + bench A/B + perf | **回退**。多提交者堆积睡眠+队列排空→谓词已真却无人唤醒；正确性必需，性能代价小且有界 |
| 2 | 2026-10-13 | worker 睡前短自旋（512×try_lock+pause）可省 futex 往返 | worker_loop 加自旋窗口 | 1w：6.14→2.86（-53%）；2w：6.00→1.26（-79%）；4w+ 全线更差 | bench A/B + perf stat | **不采纳**。CPU 饥饿环境自旋挤占生产者 CPU；仅在 CPU 富余+高到达率场景可能有效 |
| 3 | 2026-10-13 | 1 worker 档 sharded 落后 22% 是分片开销 | 隔离实验：零阻塞路径对比 + 容量对照 | 零阻塞（200 任务）：差 1.2%；sharded cap 256→4096：1w 差距 22%→9%（4.71→5.77） | bench 对照 + perf record | 归因于小容量唤醒税+流量摊薄+CPU 饥饿，非分片结构本身（详见上文 5 条） |

## gdb 取证：制造并定位一次死锁（面试弹药）

demo 用**双向原子门控**保证确定性互等（纯 sleep 版在高负载机器上会走样）：

```cpp
// deadlock_demo.cpp
#include <atomic>
#include <mutex>
#include <thread>
int main() {
    std::mutex a, b;
    std::atomic<bool> t_has_a{false}, main_has_b{false};
    std::thread t([&] {
        std::lock_guard<std::mutex> x(a);
        t_has_a.store(true);
        while (!main_has_b.load()) std::this_thread::yield();
        std::lock_guard<std::mutex> y(b);   // 持 a 等 b（此刻 b 确已被主线程持有）
    });
    while (!t_has_a.load()) std::this_thread::yield();
    std::lock_guard<std::mutex> y(b);
    main_has_b.store(true);
    std::lock_guard<std::mutex> x(a);       // 持 b 等 a —— 互等死锁
    t.join();
}
```

```bash
g++ -g -O0 deadlock_demo.cpp -o deadlock -pthread && ./deadlock   # 永久卡住
# 另开终端:
gdb -p $(pidof deadlock)
(gdb) thread apply all bt
```

实测输出摘录（Ubuntu 24.04 / gdb 15.1，2026-10-13）——两线程各自阻塞在
`pthread_mutex_lock` 的 futex 上，**等的正好是对方持有的那把锁**：

```text
Thread 2 (LWP 3898):
#0  futex_wait (futex_word=0x7fff91238170 ...)          <- 等 mutex b
#3  ___pthread_mutex_lock (mutex=0x7fff91238170)
#7  operator() (...) at deadlock_demo.cpp:14            <- lock_guard y(b)

Thread 1 (LWP 3896):
#0  futex_wait (futex_word=0x7fff91238140 ...)          <- 等 mutex a
#3  ___pthread_mutex_lock (mutex=0x7fff91238140)
#7  main () at deadlock_demo.cpp:19                     <- lock_guard x(a)
```

判读要点：两个 futex 地址不同（两把不同的锁）；每个线程都在 `lll_lock_wait`
（锁被持有者睡眠等待）；对照源码行号即得「持 a 等 b / 持 b 等 a」的互等闭环。
解法是统一加锁顺序或 `std::scoped_lock(a, b)`（内部死锁避免）。

> Windows/MinGW 旁注：本机 i686 gdb 可附加并 `thread apply all bt` 列出线程，
> 但栈回溯穿过系统 DLL 边界时断裂，用户态持锁帧不可见——符号化取证请用 Linux/WSL。

## 简历 bullet（数据已实测，按目标岗位取用）

- 实现有界队列线程池：双条件变量同步、future 异步返回、异常透传、三种拒绝策略与同步排空语义；11 条规格测试 + 混合压力测试全绿，TSan/ASan/UBSan 零告警；100 万 atomic 自增任务 2 线程吞吐 6.7M ops/s（16 vCPU WSL2 容器）
- 以 perf/对照实验驱动调优：捕获并修复条件化 notify 的丢唤醒死锁（谓词等待救不了缺失通知）；量化分片队列在本环境的负收益并五条归因（纯路径差 1.2%、小容量唤醒税、流量摊薄、vCPU 饥饿、同步原语占比 ~60%）；三条「假设→改动→数据」闭环全部成文

> 注：所有吞吐数字产自高负载宿主机上的 WSL2 容器（task-clock 仅 1.75/16 CPU），
> 复现命令 `./build/bench_pool 1000000 5`；安静机器上绝对值会显著更高，相对结论见归因分析。
