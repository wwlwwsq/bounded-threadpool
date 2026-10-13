// 规格测试：每个测试对应 README「规格 ↔ 测试对照」的一条契约。
// 骨架阶段预期全部 FAILED（not implemented）——这是 TDD 存根，不是 bug。
// 实现完成后: ./test_pool 应输出全绿并返回 0；TSan 模式下跑无告警。
#include "bpool/thread_pool.hpp"
#include "bpool/sharded_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace bpool;

static int g_fail = 0;
static int g_total = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            ++g_fail;                                                                \
            std::printf("    FAILED %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

#define CHECK_THROWS_AS(expr, Ex)                                                    \
    do {                                                                             \
        bool threw = false;                                                          \
        try {                                                                        \
            (void)(expr);                                                            \
        } catch (const Ex&) {                                                        \
            threw = true;                                                            \
        } catch (...) {                                                              \
        }                                                                            \
        if (!threw) {                                                                \
            ++g_fail;                                                                \
            std::printf("    FAILED %s:%d  期望抛出 %s\n", __FILE__, __LINE__, #Ex); \
        }                                                                            \
    } while (0)

#define RUN(fn)                                                                 \
    do {                                                                        \
        ++g_total;                                                              \
        std::printf("[ RUN      ] %s\n", #fn);                                  \
        try {                                                                   \
            fn();                                                               \
        } catch (const std::exception& e) {                                     \
            ++g_fail;                                                           \
            std::printf("    FAILED (exception) %s: %s\n", #fn, e.what());      \
        }                                                                       \
    } while (0)

static PoolOptions one_worker(size_t cap, RejectPolicy p) {
    PoolOptions o;
    o.num_workers = 1;
    o.queue_capacity = cap;
    o.policy = p;
    return o;
}

// 等 worker 把 gate 任务取走（队列腾空、worker 忙碌），使后续排布确定化
static void wait_until_taken(const ThreadPool& pool) {
    for (int i = 0; i < 2000 && pool.stats().queue_depth != 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// 1. submit 返回值经 future 传回
static void test_future_return_value() {
    ThreadPool pool(one_worker(16, RejectPolicy::kBlock));
    auto fut = pool.submit([](int a, int b) { return a + b; }, 40, 2);
    CHECK(fut.get() == 42);
}

// 2. 任务异常经 future 传递，且 worker 不死
static void test_exception_propagates_and_pool_survives() {
    ThreadPool pool(one_worker(16, RejectPolicy::kBlock));
    auto bad = pool.submit([]() -> int { throw std::runtime_error("boom"); });
    CHECK_THROWS_AS(bad.get(), std::runtime_error);
    auto ok = pool.submit([] { return 7; });
    CHECK(ok.get() == 7);
}

// 3. kThrow：队列满时 post 抛 QueueFullError 且 rejected 计数
static void test_throw_policy_rejects_when_full() {
    ThreadPool pool(one_worker(4, RejectPolicy::kThrow));
    std::promise<void> release;
    auto gate = release.get_future().share();
    pool.post([gate] { gate.get(); });
    wait_until_taken(pool);
    for (int i = 0; i < 4; ++i) pool.post([] {});  // 填满容量 4
    CHECK_THROWS_AS(pool.post([] {}), QueueFullError);
    CHECK(pool.stats().rejected >= 1);
    release.set_value();
}

// 4. kBlock 背压：队列满时 post 阻塞，腾出空位后恢复
static void test_block_policy_backpressure() {
    ThreadPool pool(one_worker(2, RejectPolicy::kBlock));
    std::promise<void> release;
    auto gate = release.get_future().share();
    pool.post([gate] { gate.get(); });
    wait_until_taken(pool);
    pool.post([] {});  // 队列 1/2
    pool.post([] {});  // 队列 2/2，满
    std::atomic<bool> third_done{false};
    std::thread blocker([&] {
        pool.post([&] { third_done = true; });  // 应阻塞在队列满
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!third_done.load());   // 仍在阻塞：队列没有空位
    release.set_value();         // gate 完成 -> 队列消费推进
    blocker.join();
    for (int i = 0; i < 2000 && !third_done.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(third_done.load());
}

// 5. kCallerRuns：任务在调用方线程同步执行
static void test_caller_runs_executes_on_caller() {
    ThreadPool pool(one_worker(1, RejectPolicy::kCallerRuns));
    std::promise<void> release;
    auto gate = release.get_future().share();
    pool.post([gate] { gate.get(); });
    wait_until_taken(pool);
    pool.post([] {});  // 填满容量 1
    std::thread::id exec_id{};
    std::atomic<bool> ran{false};
    std::thread caller([&] {
        pool.post([&] { exec_id = std::this_thread::get_id(); ran = true; });
    });
    const auto caller_id = caller.get_id();
    caller.join();  // caller-runs 语义：post 返回时任务已执行完
    CHECK(ran.load());
    CHECK(exec_id == caller_id);
    release.set_value();
}

// 6. shutdown(true) 排空：已提交任务一个不丢
static void test_shutdown_drains_all_tasks() {
    ThreadPool pool(one_worker(8, RejectPolicy::kBlock));
    constexpr int kN = 100;
    std::atomic<int> done{0};
    for (int i = 0; i < kN; ++i) pool.submit([&] { ++done; });
    pool.shutdown(true);
    CHECK(done.load() == kN);
}

// 7. 关闭后提交报错
static void test_submit_after_shutdown_throws() {
    ThreadPool pool(one_worker(4, RejectPolicy::kBlock));
    pool.shutdown(true);
    CHECK_THROWS_AS(pool.submit([] {}), PoolClosedError);
    CHECK_THROWS_AS(pool.post([] {}), PoolClosedError);
}

// 8. stats 计数
static void test_stats_counters() {
    ThreadPool pool(one_worker(64, RejectPolicy::kBlock));
    constexpr int kN = 1000;
    for (int i = 0; i < kN; ++i) pool.post([] {});
    pool.shutdown(true);
    const auto s = pool.stats();
    CHECK(s.submitted == kN);
    CHECK(s.completed == kN);
    CHECK(s.rejected == 0);
}

// 9. 并发提交压力（TSan 模式下重点看这个）
static void test_concurrent_submit_stress() {
    ThreadPool pool(PoolOptions{});
    constexpr int kThreads = 4, kPer = 500;
    std::atomic<long long> sum{0};
    {
        std::vector<std::thread> producers;
        for (int t = 0; t < kThreads; ++t)
            producers.emplace_back([&] {
                try {  // 骨架阶段 post 抛 not implemented，兜住防止 terminate
                    for (int i = 0; i < kPer; ++i)
                        pool.post([&] { sum.fetch_add(1, std::memory_order_relaxed); });
                } catch (const std::exception&) {
                }
            });
        for (auto& th : producers) th.join();
    }
    pool.shutdown(true);
    CHECK(sum.load() == static_cast<long long>(kThreads) * kPer);
}

// 10. 分片版功能等价
static void test_sharded_pool_equivalent() {
    ShardedThreadPool pool(PoolOptions{}, 4);
    auto fut = pool.submit([](int x) { return x * x; }, 9);
    CHECK(fut.get() == 81);
    std::atomic<int> done{0};
    for (int i = 0; i < 200; ++i) pool.post([&] { ++done; });
    pool.shutdown(true);
    CHECK(done.load() == 200);
}

int main() {
    RUN(test_future_return_value);
    RUN(test_exception_propagates_and_pool_survives);
    RUN(test_throw_policy_rejects_when_full);
    RUN(test_block_policy_backpressure);
    RUN(test_caller_runs_executes_on_caller);
    RUN(test_shutdown_drains_all_tasks);
    RUN(test_submit_after_shutdown_throws);
    RUN(test_stats_counters);
    RUN(test_concurrent_submit_stress);
    RUN(test_sharded_pool_equivalent);
    std::printf("\ntests: %d, failed checks: %d%s\n", g_total, g_fail,
                g_fail == 0 ? "  —— 全绿" : "");
    return g_fail ? 1 : 0;
}
