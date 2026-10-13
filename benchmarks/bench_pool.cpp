// 吞吐基准：任务体 = 一次 atomic 自增（衡量调度开销本身，而非任务耗时）。
// 用法: ./bench_pool [任务数]   （默认 100 万）
// 输出 markdown 表行，直接粘进 README 的数据表。
#include "bpool/thread_pool.hpp"
#include "bpool/sharded_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace bpool;
using clk = std::chrono::steady_clock;

// 轮询等待全部完成（每 64 次 yield 查一次 stats，避免与 worker 抢锁）
template <class Pool>
static void wait_all(Pool& pool, uint64_t ntasks) {
    while (pool.stats().completed < ntasks) {
        for (int i = 0; i < 64; ++i) std::this_thread::yield();
    }
}

template <class Pool>
static void bench(const char* name, int workers, size_t cap, size_t ntasks) {
    PoolOptions o;
    o.num_workers = workers;
    o.queue_capacity = cap;  // 分片版语义为「每 shard 容量」
    o.policy = RejectPolicy::kBlock;
    Pool pool(o);
    std::atomic<uint64_t> done{0};
    const auto t0 = clk::now();
    for (size_t i = 0; i < ntasks; ++i)
        pool.post([&] { done.fetch_add(1, std::memory_order_relaxed); });
    wait_all(pool, ntasks);
    const double dt = std::chrono::duration<double>(clk::now() - t0).count();
    (void)done;
    std::printf("| %s | %d | %zu | %.2f |\n", name, workers, cap,
                double(ntasks) / dt / 1e6);
}

int main(int argc, char** argv) {
    const size_t ntasks = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;

    std::vector<int> ws{1, 2, 4, 8};
    const int hc = static_cast<int>(std::thread::hardware_concurrency());
    if (hc > 8) ws.push_back(hc);

    std::printf("| pool | workers | queue_cap(shard) | M ops/s |\n|---|---|---|---|\n");
    for (int w : ws) {
        bench<ThreadPool>("baseline", w, 4096, ntasks);
        bench<ShardedThreadPool>("sharded", w, 256, ntasks);
    }
    // TODO(进阶): P99 延迟——采样任务记录 submit->执行 的时间戳（每 64 个采 1 个），
    //             输出 p50/p99；对比两种池在高水位队列下的尾延迟差异
    return 0;
}
