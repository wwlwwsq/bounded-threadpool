#include "bpool/sharded_pool.hpp"

#include <algorithm>
#include <stdexcept>

namespace bpool {

ShardedThreadPool::ShardedThreadPool(PoolOptions opts, size_t num_shards)
    : opts_(std::move(opts)) {
    if (opts_.num_workers == 0)
        throw std::invalid_argument("num_workers must be >= 1");
    if (opts_.queue_capacity == 0)
        throw std::invalid_argument("queue_capacity must be >= 1");
    const size_t nshards = num_shards ? num_shards : opts_.num_workers;
    // D6: 每个 shard 至少要有一个消费者，否则无 worker 的 shard 任务永远不被执行，
    //     drain 语义被破坏——worker 数下限抬到 shard 数
    const size_t nworkers = std::max(opts_.num_workers, nshards);
    shards_.reserve(nshards);
    for (size_t i = 0; i < nshards; ++i)
        shards_.push_back(std::make_unique<Shard>());
    workers_.reserve(nworkers);
    try {
        for (size_t i = 0; i < nworkers; ++i) {
            {
                std::lock_guard<std::mutex> lk(mu_shutdown_);
                ++active_workers_;  // 先计数再起线程，worker 退出时的减一才能配对
            }
            try {
                // worker 只消费绑定的 shard，跨 shard 不偷任务（D4，简化模型）
                workers_.emplace_back([this, i, nshards] { worker_loop(i % nshards); });
            } catch (...) {
                { std::lock_guard<std::mutex> lk(mu_shutdown_); --active_workers_; }
                throw;
            }
        }
    } catch (...) {
        closed_.store(true, std::memory_order_seq_cst);
        for (auto& sh : shards_) {
            std::lock_guard<std::mutex> lk(sh->mu);
            sh->cv_not_empty.notify_all();
            sh->cv_not_full.notify_all();
        }
        for (auto& t : workers_)
            if (t.joinable()) t.join();
        throw;
    }
}

ShardedThreadPool::~ShardedThreadPool() {
    shutdown(true);
    for (auto& t : workers_)
        if (t.joinable()) t.join();
}

ShardedThreadPool::Shard& ShardedThreadPool::pick_shard() {
    return *shards_[rr_.fetch_add(1, std::memory_order_relaxed) % shards_.size()];
}

void ShardedThreadPool::worker_loop(size_t shard_idx) {
    Shard& s = *shards_[shard_idx];
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(s.mu);
            s.cv_not_empty.wait(lk, [&] {
                return !s.tasks.empty() || closed_.load(std::memory_order_relaxed);
            });
            if (closed_.load(std::memory_order_relaxed) &&
                (cancel_.load(std::memory_order_relaxed) || s.tasks.empty()))
                break;
            const bool was_full = s.tasks.size() >= opts_.queue_capacity;
            task = std::move(s.tasks.front());
            s.tasks.pop_front();
            if (was_full) s.cv_not_full.notify_one();
        }
        try {
            task();
        } catch (...) {
        }
        completed_.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lk(mu_shutdown_);
        --active_workers_;
    }
    cv_shutdown_.notify_all();
}

void ShardedThreadPool::shutdown(bool drain) {
    // 先置 closed_（原子），再逐 shard 持锁清队列 + notify：
    // 与 post 的「锁内查 closed_」配合，保证不会有关闭后入队的任务漏掉
    closed_.store(true, std::memory_order_seq_cst);
    if (!drain) cancel_.store(true, std::memory_order_seq_cst);
    for (auto& sh : shards_) {
        std::lock_guard<std::mutex> lk(sh->mu);
        if (!drain) sh->tasks.clear();  // D5: 丢弃任务的 future 立即变为 broken_promise
        sh->cv_not_empty.notify_all();
        sh->cv_not_full.notify_all();
    }
    if (drain) {
        // 同 D7：drain=true 同步等到排空完成；不得在任务体内调用（自等死锁）
        std::unique_lock<std::mutex> lk(mu_shutdown_);
        cv_shutdown_.wait(lk, [&] { return active_workers_ == 0; });
    }
}

PoolStats ShardedThreadPool::stats() const {
    PoolStats s;
    s.submitted = submitted_.load(std::memory_order_relaxed);
    s.completed = completed_.load(std::memory_order_relaxed);
    s.rejected  = rejected_.load(std::memory_order_relaxed);
    for (const auto& sh : shards_) {
        std::lock_guard<std::mutex> lk(sh->mu);
        s.queue_depth += static_cast<uint64_t>(sh->tasks.size());
    }
    return s;
}

}  // namespace bpool
