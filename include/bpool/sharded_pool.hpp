// bounded-threadpool：分片队列线程池（优化版）
// 动机（这就是简历上的「性能调优」故事）：
//   单队列单锁在 worker 数增多时锁竞争成为瓶颈；每个 shard 拥有独立的
//   mutex + 条件变量，submit 轮转选片，锁竞争被摊薄。
//   用 bench_pool 对比 baseline / sharded 的吞吐曲线即优化证据。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "thread_pool.hpp"

namespace bpool {

class ShardedThreadPool {
public:
    // num_shards 为 0 时取 num_workers
    explicit ShardedThreadPool(PoolOptions opts = {}, size_t num_shards = 0);
    ~ShardedThreadPool();

    ShardedThreadPool(const ShardedThreadPool&) = delete;
    ShardedThreadPool& operator=(const ShardedThreadPool&) = delete;

    template <class F>
    void post(F&& f);

    template <class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>;

    void shutdown(bool drain = true);

    PoolStats stats() const;

private:
    struct Shard {
        std::mutex mu;
        std::condition_variable cv_not_empty;
        std::condition_variable cv_not_full;
        std::deque<std::function<void()>> tasks;
    };

    void worker_loop(size_t shard_idx);
    Shard& pick_shard();

    PoolOptions opts_;  // queue_capacity 语义：每个 shard 的容量
    std::vector<std::unique_ptr<Shard>> shards_;
    std::vector<std::thread> workers_;

    mutable std::mutex mu_shutdown_;        // 只保护 active_workers_，不进热路径
    std::condition_variable cv_shutdown_;   // worker 全部退出，唤醒等待排空的 shutdown(true)
    size_t active_workers_ = 0;

    std::atomic<bool> closed_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<uint64_t> rr_{0};  // 轮转选片计数

    std::atomic<uint64_t> submitted_{0};
    std::atomic<uint64_t> completed_{0};
    std::atomic<uint64_t> rejected_{0};
};

// ---------- 模板实现 ----------

template <class F>
void ShardedThreadPool::post(F&& f) {
    Shard& s = pick_shard();
    std::unique_lock<std::mutex> lk(s.mu);
    if (closed_.load(std::memory_order_relaxed))
        throw PoolClosedError("post on closed pool");
    if (s.tasks.size() >= opts_.queue_capacity) {
        switch (opts_.policy) {
        case RejectPolicy::kThrow:
            rejected_.fetch_add(1, std::memory_order_relaxed);
            throw QueueFullError("queue full");
        case RejectPolicy::kBlock:
            // 同 D1：谓词含 closed_，shutdown 后醒来抛 PoolClosedError
            s.cv_not_full.wait(lk, [&] {
                return s.tasks.size() < opts_.queue_capacity ||
                       closed_.load(std::memory_order_relaxed);
            });
            if (closed_.load(std::memory_order_relaxed))
                throw PoolClosedError("post on closed pool");
            break;
        case RejectPolicy::kCallerRuns:
            lk.unlock();
            submitted_.fetch_add(1, std::memory_order_relaxed);
            std::forward<F>(f)();
            completed_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    s.tasks.emplace_back(std::forward<F>(f));
    submitted_.fetch_add(1, std::memory_order_relaxed);
    s.cv_not_empty.notify_one();
}

template <class F, class... Args>
auto ShardedThreadPool::submit(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
    using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
    auto task = std::make_shared<std::packaged_task<R()>>(
        [fn = std::forward<F>(f),
         tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            return std::apply(std::move(fn), std::move(tup));
        });
    auto fut = task->get_future();
    post([task] { (*task)(); });
    return fut;
}

}  // namespace bpool
