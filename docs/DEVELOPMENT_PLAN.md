# bounded-threadpool 开发计划与验收标准

> 制定日期：2026-10-13 ｜ 基线 commit：`4b4c3aa`（TDD 骨架，测试全红）
> 本文档是本项目唯一的推进依据：每个里程碑有**可机器验证的退出条件**，五道验收关卡全部通过才算完成。
> 配套：根目录 README（使用/规格对照/设计决策/数据表）。文末「变更记录」登记对本文的每次修订。

---

## 0. 项目定位与成功定义

**定位**：面向目标 JD（高并发 / 稳定性 / 性能调优 / Linux+gdb）的可复现证据链项目。
面试官打开这个 repo，能在 3 分钟内看到：测试全绿的规格实现、TSan/ASan 取证、perf 驱动的优化前后对比数据。

**成功定义**（全部满足）：

1. 五道验收关卡（§4 A–E）全部通过；
2. README 数据表无空项，优化记录 ≥ 2 条完整闭环；
3. 全新环境 clone 后按 README 命令一键复现全部结果；
4. 每个设计决策点（§3 D1–D5）都能对着代码讲 3 分钟。

---

## 1. 现状基线

| 项 | 状态 |
|---|---|
| 代码骨架 | ~~接口/规格测试/基准/脚本齐备，TODO(1)–(13) 未实现~~ → **TODO(1)–(13) 已全部实现**（2026-10-13） |
| 测试 | ~~test_pool 10/10 红~~ → **test_pool 11/11 绿**（新增 1 条 D5 用例，见变更记录）；连续 20 次通过；UBSan trap 模式通过 |
| 环境 | Windows + MinGW g++ 15.2（i686/32 位，无 cmake，已用 g++ 直连验证）；**WSL Ubuntu 未装**（TSan/perf/gdb 取证必需，M3/M4 的 Linux 部分仍被阻塞） |
| 远端 | github.com/wwlwwsq/bounded-threadpool @ main = 4b4c3aa |

> 本机验证命令见 README「构建」：g++ 直连（无 cmake）+ `-fsanitize=undefined -fsanitize-undefined-trap-on-error`（MinGW 无 libasan/libubsan/TSan）。

---

## 2. 里程碑总览（5.5 个工作日 + 1 天 buffer）

| 里程碑 | 内容 | 退出条件（可验证） | 预计 |
|---|---|---|---|
| **M0 环境** | WSL Ubuntu + 工具链 + 首次构建 | `cmake --build build` 成功；`./build/test_pool` 可执行且全绿 | 0.5d |
| **M1 基础池** | TODO(1)–(6)：post/submit/生命周期/worker 循环 | Release 下 `test_pool` **11/11 绿**（退出码 0；本机 MinGW 已达成，WSL 待复核） | 1.5d ✅ |
| **M2 分片池** | TODO(7)–(13)：分片版同语义实现 | 分片用例绿；`bench_pool` 双池数据出表（本机已出临时表，WSL 数据为证据版） | 1d ✅ |
| **M3 正确性取证** | ASan/UBSan + TSan + gdb 演练 | 三模式测试退出码 0、TSan 零 WARNING；gdb 死锁笔记成文 | 0.5d |
| **M4 性能取证** | 基准矩阵 + 优化迭代 + 优化记录 | §4 关卡 C 的基准矩阵填满；优化记录 ≥2 条闭环 | 1.5d |
| **M5 收尾** | 文档/决策记录/复现检查/打 tag | §4 关卡 D 全过；`git tag v1.0.0` | 0.5d |

规则：**里程碑按序推进，上一级退出条件不达标不进下一级**；提前完成才允许启动 §6 加分项。

---

## 3. 任务分解与设计决策点

### M1 基础池（TODO(1)–(6)，头文件模板 → 生命周期 → worker 循环）

| 任务 | 涉及 | 转绿的测试 |
|---|---|---|
| TODO(1) post 基本路径 + kThrow/kBlock/kCallerRuns | thread_pool.hpp | test_throw_policy / test_block_policy / test_caller_runs |
| TODO(2) submit 的 packaged_task 包装 | thread_pool.hpp | test_future_return_value / test_exception_propagates |
| TODO(3) 构造：参数校验 + 启动 worker | thread_pool.cpp | （间接） |
| TODO(4) 析构：隐式 shutdown(true) + join | thread_pool.cpp | test_shutdown_drains（无悬挂线程） |
| TODO(5) shutdown(drain) 流程 | thread_pool.cpp | test_shutdown_drains / test_submit_after_shutdown / test_shutdown_discard_breaks_futures |
| TODO(6) worker 主循环（wait/pop/notify_not_full/吞异常/completed++） | thread_pool.cpp | test_stats_counters / test_concurrent_stress |

### M2 分片池（TODO(7)–(13)）

| 任务 | 涉及 |
|---|---|
| TODO(7)(8) 分片版 post/submit 模板（只锁目标 shard） | sharded_pool.hpp |
| TODO(9) 构造：shards 数 = num_shards ? : num_workers，worker 绑定单 shard | sharded_pool.cpp |
| TODO(10) 析构 + join | sharded_pool.cpp |
| TODO(11) pick_shard：轮转（进阶版选最浅，见 §6） | sharded_pool.cpp |
| TODO(12) 分片 worker 循环 | sharded_pool.cpp |
| TODO(13) 分片 shutdown：逐 shard notify_all | sharded_pool.cpp |

退出测试：test_sharded_pool_equivalent 绿；随后 `./build/bench_pool 1000000` 首次出双池对比表。

### 设计决策点（已全部定稿，正文见 README「设计决策」节——面试讲解弹药）

- **D1 关闭时阻塞中的提交者**：kBlock 的 post 在 `closed_` 后醒来必须抛 `PoolClosedError`，任务不入队不执行——"丢任务才是正确行为"（否则 shutdown 永远等不到，或排空契约被破坏）。
- **D2 caller-runs 计数语义**（已定稿）：`submitted++` 与 `completed++` 都计——completed 的字面语义是「执行完毕的任务数」，与执行线程无关。
- **D3 为什么 worker 在 pop 之后（而非执行完之后）notify_not_full**：让生产者尽早拿到空位，任务执行期间生产者可并行入队。**每次 pop 必须 notify_one 与空位一一配对**：「仅队列原本满时才通知」的省调用优化会被 stress_mix 捕获死锁（多提交者堆积睡眠→队列排空→谓词已真却无人唤醒）——谓词等待救不了缺失的通知（2026-10-13 实测踩坑，已回退）。
- **D4 分片不偷任务**：worker 只消费绑定 shard，队头倾斜时负载不均——量化说明这个简化在什么负载下可接受（bench 数据支撑，M4）。
- **D5 shutdown(false) 丢弃任务的 future 状态**（修正后定稿）：**必须在 shutdown(false) 内就地清空队列**，被丢弃任务的 packaged_task 随之销毁、future 立即变 `broken_promise`。原方案只写了"走 packaged_task 销毁"，但骨架代码不清队时 future 要等到**池析构**才打破、期间 `get()` 永久阻塞——已实现修正并加 test_shutdown_discard_breaks_futures 机器验证。
- **D6（新增）每个 shard 至少一个 worker**：`num_shards > num_workers` 时无消费者的 shard 任务永不执行、drain 卡死；worker 数取 `max(num_workers, num_shards)`。
- **D7（新增）shutdown 同步语义**：drain=true 阻塞到「已入队任务全部完成、worker 全部退出」才返回（`cv_shutdown_` + `active_workers_`）——否则"排空不丢任务"无法被调用方观测（test_shutdown_drains_all_tasks 直接依赖此语义，实现时曾暴露该缺口）；drain=false 原子清队后立即返回，运行中任务由析构 join 收尾。约束：不得在任务体内调用 shutdown(true)（自等死锁）。

---

## 4. 验收标准（五道关卡）

### 关卡 A · 功能

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/test_pool
```
- 退出码 0，输出 `tests: 11, failed checks: 0  —— 全绿`；
- 11 条用例对应 README「规格 ↔ 测试对照」逐条成立。

### 关卡 B · 正确性与稳定性（Linux/WSL 下执行）

```bash
bash scripts/build_and_test.sh   # 常规 + ASan+UBSan + TSan 三连（已修复：任一模式失败则脚本退出码非 0）
```
- ASan+UBSan 模式：退出码 0，无泄漏/UB 报告；
- TSan 模式：退出码 0，**零 WARNING**（重点盯 test_concurrent_submit_stress 与 stress_mix）；
- 稳定性：`test_concurrent_submit_stress` 连续 20 次无一次失败（2026-10-13 本机 MinGW 已验证 20/20；WSL 待复核）；
- 线程回收：任一测试进程结束后 `cat /proc/<pid>/status` 不可查即视为退出（或加一条"析构后 threads 全 join"断言）；
- gdb 死锁演练笔记成文（README 已给命令，产出贴进 README「gdb 取证」节，含 `thread apply all bt` 输出摘录）。

> 脚本行为（2026-10-13 修复）：非 Linux/macOS 环境自动跳过 asan/tsan 模式（MinGW 无对应运行时），只跑常规模式。
> Windows 本机替代取证：`-fsanitize=undefined -fsanitize-undefined-trap-on-error`（trap 模式，已通过）。

### 关卡 C · 性能（数据说话，不设拍脑袋的硬指标）

基准矩阵（全部实测填入 README 数据表）：

| 维度 | 取值 |
|---|---|
| pool | baseline（单锁） / sharded（分片） |
| workers | 1, 2, 4, 8, hardware_concurrency |
| 任务数 | 1,000,000（任务体 = atomic 自增） |
| reps | ≥ 5 取中位 |

**硬性要求**：
1. 矩阵每格有数，1 线程行两池差距 < 5%（ sanity check，否则先查实现 bug）；
2. sharded 在 ≥4 workers 档**不低于** baseline；若未达到，必须在「优化记录」写出基于 perf 数据的归因分析（什么占了瓶颈、为什么分片没收益）——**有数据的负结论也算过，空格子不算**；
3. 优化记录 ≥ 2 条完整闭环：`假设 → 改动 → 前后数据 → 工具`；
4. perf `record/report` 至少各一次留痕（热点函数截图或文本贴 README）。

> 取证环境限定 WSL2/x86-64：本机 Windows/32 位 i686 MinGW 实测不满足 sanity check 且 run-to-run 方差巨大
> （归因假设：i686 上 `atomic<uint64_t>` 走 lock cmpxchg8b + winpthreads cv 开销，详见 README 数据表注记），
> 该环境数据只作临时参考，不进证据链。

### 关卡 D · 文档与可复现

1. README 数据表、优化记录、设计决策（D1–D5 至少写 4 条）无占位符残留（`grep -n "__" README.md` 为空）；
2. 全新 WSL 环境：`git clone` → 按 README「构建」三步命令跑通测试与基准；
3. 本计划 §2 各里程碑的退出条件逐条可勾选。

### 关卡 E · 提交卫生

1. 每个里程碑 ≥ 1 次任务级粒度 commit（M1 允许 3–5 个小提交）；
2. commit message 动词开头、说明动机（例：`worker 循环: pop 后立即 notify_not_full, 生产者提前拿空位`）；
3. 完成时打 tag：`git tag -a v1.0.0 -m "全部验收关卡通过"` 并推送。

---

## 5. 风险与预案

| 风险 | 概率 | 预案 |
|---|---|---|
| 时序类测试 flaky（backpressure/caller-runs） | 中 | 已用 promise-gate 门控确定性；仍 flaky 就把 sleep 换成条件轮询，禁用"加大 sleep"这种糊法 |
| 分片在笔记本上无提升 | 中高 | 属合法负结论：加任务负载（每任务多几次原子加）或提升任务数到 200 万再测；仍无则写归因 |
| WSL2 perf 不可用 | 中 | 换 valgrind --tool=callgrind（注明等价性）；或物理机/云主机跑取证 |
| TSan 报唤醒丢失/虚假唤醒 | 低 | 检查是否全部用谓词 wait；notify 在持锁/放锁后均可，但要保证修改谓词与 notify 不重排出错 |
| 时间超支 | 中 | 优先级：M1–M3 硬关卡 > M4 完整矩阵 > M5 文案；§6 加分项全砍 |

---

## 6. 加分项 backlog（仅当 §4 全过后启动，严防范围蔓延）

- P99 延迟探针（bench_pool 已留 TODO）：对比两池高水位队列下的尾延迟；
- pick_shard 选最浅 vs 轮转的对比数据；
- 与开源 moodycamel::ConcurrentQueue 的吞吐对比（引用第三方库，说明定位差异）；
- work-stealing 跨 shard 偷任务（推翻 D4，用数据说话）。

**明确不做**（写清楚防止自我膨胀）：动态线程数、任务优先级、跨平台 IO 多路复用集成、协程。

---

## 7. 面试讲解映射（JD 关键词 → 本项目证据 → 3 分钟讲稿要点）

| JD 关键词 | 证据 | 讲稿要点 |
|---|---|---|
| 高并发 | 有界队列 + 三种拒绝策略 + 背压 | 为什么无界队列会 OOM、backpressure 怎么救生产者-消费者 |
| 稳定性 | 异常透传、优雅关闭、TSan/ASan | 任务抛异常 worker 为什么不死；shutdown 排空语义 |
| 性能调优 | perf 热点 → 分片队列 → 前后数据 | 锁竞争摊薄、临界区缩小（pop 后执行）、Amdahl 上限 |
| Linux/gdb | 全程 WSL 开发 + 死锁排查笔记 | `thread apply all bt` 抓互等现场、core dump 分析 |
| C++ 功底 | cv 谓词等待、packaged_task/future、RAII | 虚假唤醒、notify 时机、broken_promise |

---

## 8. 时间表（具体到半天，起始日 = M0 当天）

| 时段 | 内容 | 产出 |
|---|---|---|
| D1 上午 | M0：WSL Ubuntu + 工具链 + 构建 | 构建日志 commit |
| D1 下午 ~ D2 | M1：TODO(1)–(6) | test_pool 10/10 绿 |
| D3 上午 | M2：TODO(7)–(13) | 分片用例绿 + 首张对比表 |
| D3 下午 | M3：三模式取证 + gdb 演练 | TSan 零告警 + 死锁笔记 |
| D4 全天 | M4：基准矩阵 + 优化迭代 | 数据表全填 + 优化记录 ≥2 |
| D5 上午 | M5：文档/复现检查/tag | v1.0.0 |
| D5 下午 + buffer | 加分项（可选） / 简历数字回填 | — |

> 推进方式：完成一项勾一项；本文件改动（如决策定稿、风险兑现）随代码一起 commit。

---

## 9. 变更记录

### 2026-10-13（实现日：M1 + M2 完成，本机验证通过）

**方案修正（对本文档的错误/缺口修正）**：

1. **D5 语义修正**：原文「走 packaged_task 销毁 → broken_promise」在骨架代码（shutdown 不清队）下不成立——future 要等到**池析构**才打破，期间 `get()` 永久阻塞。定稿：`shutdown(false)` 必须就地清空队列。新增第 11 条测试 `test_shutdown_discard_breaks_futures` 机器验证（测试计数 10 → 11，关卡 A/M1 退出条件同步改为 11/11）。
2. **D7 新增（shutdown 同步语义缺口）**：原 TODO(5) 未规定 shutdown 是否等待排空。实现时 test_shutdown_drains_all_tasks / test_stats_counters 暴露该缺口：drain 型 shutdown 必须阻塞到「任务全部完成 + worker 全部退出」才返回，否则"排空不丢任务"无法被调用方观测。定稿：drain=true 同步等待，drain=false 立即返回。
3. **D6 新增（分片消费者覆盖）**：原 TODO(9) 未处理 `num_shards > num_workers` 的情形——无 worker 绑定的 shard 任务永不执行、drain 卡死。定稿：worker 数取 `max(num_workers, num_shards)`。
4. **关卡 B 脚本 bug**：`build_and_test.sh` 原版任何情况下退出码都是 0，「退出码 0」这一验收条件永远不可能失败。已修复为聚合失败并以非 0 退出；同时非 Linux/macOS 自动跳过 asan/tsan（MinGW 无 libasan/libubsan/TSan，实测 `cannot find -lasan/-lubsan`）。
5. **D2 定稿**：caller-runs 任务 `submitted++` 与 `completed++` 都计（completed 语义 = 执行完毕的任务数，与线程无关）。
6. **环境注记**：本机为 32 位 i686 MinGW（无 cmake），M1/M2 退出条件改经 g++ 直连验证；M0 的 cmake/WSL 部分顺延，与 M3（TSan/ASan/gdb）、M4（perf/基准矩阵）一并构成剩余待办。

**本机验证结果（Windows / MinGW g++ 15.2 i686）**：

- `test_pool`：11/11 全绿（Release `-O2`），退出码 0；
- 稳定性：连续 20 次运行 20 次通过；
- UB 取证：`-fsanitize=undefined -fsanitize-undefined-trap-on-error` 模式 11/11 全绿（无 trap）；
- 基准：双池全矩阵可跑通，数据已录 README（标注临时参考 + 归因假设，x86-64 WSL 复测后才进证据链）。

**剩余待办**：M0（WSL + cmake 构建复核）→ M3（ASan+UBSan / TSan 零告警 / gdb 笔记）→ M4（perf + 基准矩阵 + ≥2 条优化闭环）→ M5（文档终检 + v1.0.0 tag）。
