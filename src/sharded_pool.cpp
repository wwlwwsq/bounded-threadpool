#include "bpool/sharded_pool.hpp"

namespace bpool {

ShardedThreadPool::ShardedThreadPool(PoolOptions opts, size_t num_shards)
    : opts_(std::move(opts)) {
    (void)num_shards;
    // TODO(9) shards_ 数量 = num_shards ? num_shards : opts_.num_workers
    //         每个 worker 绑定一个 shard 跑 worker_loop(i)
    //         注意：worker 只消费自己绑定的 shard，跨 shard 不偷任务（简化模型，写进 README 讨论）
}

ShardedThreadPool::~ShardedThreadPool() {
    // TODO(10) 未关闭则 shutdown(true)；join 所有 worker
}

ShardedThreadPool::Shard& ShardedThreadPool::pick_shard() {
    // TODO(11) 轮转：rr_.fetch_add(1) % shards_.size()
    //          进阶：比较各 shard 深度选最浅（多一次锁开销，值不值？跑数据说话）
    return *shards_.at(0);
}

void ShardedThreadPool::worker_loop(size_t shard_idx) {
    (void)shard_idx;
    // TODO(12) 与 ThreadPool::worker_loop 逻辑相同，但只操作 shards_[shard_idx] 的队列与 cv
}

void ShardedThreadPool::shutdown(bool drain) {
    (void)drain;
    // TODO(13) 置位 closed_ / (drain ? 不动 : cancel_)，然后逐 shard notify_all 两个 cv
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
