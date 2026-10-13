// 混合压力测试：多生产者随机混合 post/submit × 三种拒绝策略 × 并发 shutdown（含 drain=false 丢弃）。
// 这是 TSan 模式下除 test_pool 外的重点观察对象。
//
// 守恒律（drain=true 时逐轮断言）：每次提交尝试落入且仅落入三个桶之一——
//   completed（入队后执行 / caller-runs 同步执行）、rejected（kThrow 满队）、
//   closed（并发 shutdown 竞争中输掉，D1 路径）。
// drain=false 轮只断言不挂死、计数不越界（丢弃是合法结局）。
#include "bpool/thread_pool.hpp"
#include "bpool/sharded_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace bpool;

static int g_fail = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            ++g_fail;                                                               \
            std::printf("    FAILED %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                            \
    } while (0)

struct Buckets {
    std::atomic<uint64_t> accepted{0};  // post/submit 成功（含 caller-runs）
    std::atomic<uint64_t> rejected{0};  // QueueFullError
    std::atomic<uint64_t> closed{0};    // PoolClosedError（并发 shutdown 竞争输掉）
};

static PoolOptions make_opts(size_t workers, size_t cap, RejectPolicy p) {
    PoolOptions o;
    o.num_workers = workers;
    o.queue_capacity = cap;
    o.policy = p;
    return o;
}

template <class Pool>
static void run_round(Pool& pool, int nthreads, int per, int seed,
                      bool shutdown_during, bool drain, const char* tag) {
    std::fprintf(stderr, "[ RUN      ] %s\n", tag);
    Buckets b;
    std::vector<std::future<int>> futs;
    std::mutex futs_mu;
    std::thread closer;
    if (shutdown_during)
        closer = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            pool.shutdown(drain);
        });

    std::vector<std::thread> ths;
    for (int t = 0; t < nthreads; ++t)
        ths.emplace_back([&, t] {
            std::mt19937 rng(static_cast<unsigned>(seed * 97 + t));
            for (int i = 0; i < per; ++i) {
                try {
                    if (rng() % 4 == 0) {
                        auto fut = pool.submit([] { return 1; });
                        {
                            std::lock_guard<std::mutex> lk(futs_mu);
                            futs.push_back(std::move(fut));
                        }
                    } else {
                        pool.post([] {});
                    }
                    b.accepted.fetch_add(1, std::memory_order_relaxed);
                } catch (const QueueFullError&) {
                    b.rejected.fetch_add(1, std::memory_order_relaxed);
                } catch (const PoolClosedError&) {
                    b.closed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    for (auto& th : ths) th.join();
    if (shutdown_during)
        closer.join();
    else
        pool.shutdown(drain);  // 生产者全部结束后再关：closed 桶应为 0

    const uint64_t attempts = static_cast<uint64_t>(nthreads) * per;
    const auto s = pool.stats();
    CHECK(b.accepted.load() + b.rejected.load() + b.closed.load() == attempts);
    CHECK(s.submitted == b.accepted.load());
    CHECK(s.rejected == b.rejected.load());
    if (drain) {
        CHECK(s.completed == b.accepted.load());  // 排空：成功提交的全部执行
        if (!shutdown_during) CHECK(b.closed.load() == 0);
    } else {
        CHECK(s.completed <= b.accepted.load());  // 丢弃合法，但不能凭空多执行
    }
    // 成功 submit 的 future：drain 必得 1；丢弃轮允许 broken_promise
    for (auto& f : futs) {
        try {
            CHECK(f.get() == 1);
        } catch (const std::future_error&) {
            CHECK(!drain);
        }
    }
    // stderr 无缓冲：挂死时能看到卡在哪一轮
    std::fprintf(stderr, "[       OK ] %s (accepted=%llu rejected=%llu closed=%llu)\n", tag,
                 static_cast<unsigned long long>(b.accepted.load()),
                 static_cast<unsigned long long>(b.rejected.load()),
                 static_cast<unsigned long long>(b.closed.load()));
}

int main() {
    constexpr int kThreads = 4, kPer = 400;
    const RejectPolicy policies[] = {RejectPolicy::kBlock, RejectPolicy::kThrow,
                                     RejectPolicy::kCallerRuns};
    int round = 0;
    for (RejectPolicy p : policies) {
        for (size_t cap : {size_t(4), size_t(64)}) {
            char tag[64];
            // 基线池：生产者结束后 drain 关闭（无 closed 竞争）
            {
                std::snprintf(tag, sizeof tag, "baseline p=%d cap=%zu drain-after", int(p), cap);
                ThreadPool pool(make_opts(4, cap, p));
                run_round(pool, kThreads, kPer, ++round, false, true, tag);
            }
            // 基线池：并发 drain 关闭（D1：阻塞中的提交者被唤醒后抛 PoolClosedError）
            {
                std::snprintf(tag, sizeof tag, "baseline p=%d cap=%zu drain-race", int(p), cap);
                ThreadPool pool(make_opts(4, cap, p));
                run_round(pool, kThreads, kPer, ++round, true, true, tag);
            }
            // 基线池：并发丢弃关闭（D5：future 允许 broken_promise）
            {
                std::snprintf(tag, sizeof tag, "baseline p=%d cap=%zu discard-race", int(p), cap);
                ThreadPool pool(make_opts(4, cap, p));
                run_round(pool, kThreads, kPer, ++round, true, false, tag);
            }
            // 分片池：同样三个场景
            {
                std::snprintf(tag, sizeof tag, "sharded  p=%d cap=%zu drain-after", int(p), cap);
                ShardedThreadPool pool(make_opts(4, cap, p), 4);
                run_round(pool, kThreads, kPer, ++round, false, true, tag);
            }
            {
                std::snprintf(tag, sizeof tag, "sharded  p=%d cap=%zu drain-race", int(p), cap);
                ShardedThreadPool pool(make_opts(4, cap, p), 4);
                run_round(pool, kThreads, kPer, ++round, true, true, tag);
            }
            {
                std::snprintf(tag, sizeof tag, "sharded  p=%d cap=%zu discard-race", int(p), cap);
                ShardedThreadPool pool(make_opts(4, cap, p), 4);
                run_round(pool, kThreads, kPer, ++round, true, false, tag);
            }
        }
    }
    // 分片池 shard 数 > worker 数：D6 路径（worker 数抬升到 shard 数）
    {
        ShardedThreadPool pool(make_opts(2, 8, RejectPolicy::kBlock), 5);
        run_round(pool, kThreads, kPer, ++round, false, true, "sharded  shards>workers");
    }
    std::printf("\nstress_mix: %s\n", g_fail ? "FAILED" : "全绿");
    return g_fail ? 1 : 0;
}
