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

    std::atomic<bool> closed_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<uint64_t> rr_{0};  // 轮转选片计数

    std::atomic<uint64_t> submitted_{0};
    std::atomic<uint64_t> completed_{0};
    std::atomic<uint64_t> rejected_{0};
};

// ---------- 模板实现（TODO 存根） ----------

template <class F>
void ShardedThreadPool::post(F&& f) {
    (void)f;
    // TODO(7) 与 ThreadPool::post 相同语义，但只锁目标 shard：
    //   Shard& s = pick_shard();
    //   后续步骤同 TODO(1)，锁与 cv 全部换成 s 的成员
    throw std::logic_error("ShardedThreadPool::post: not implemented");
}

template <class F, class... Args>
auto ShardedThreadPool::submit(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
    (void)f;
    (static_cast<void>(args), ...);
    // TODO(8) 同 ThreadPool::submit 的 packaged_task 包装，但走本类的 post
    throw std::logic_error("ShardedThreadPool::submit: not implemented");
}

}  // namespace bpool
