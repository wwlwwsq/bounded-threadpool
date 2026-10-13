#include "bpool/thread_pool.hpp"

namespace bpool {

ThreadPool::ThreadPool(PoolOptions opts) : opts_(std::move(opts)) {
    // TODO(3) 校验 num_workers >= 1、queue_capacity >= 1（否则 std::invalid_argument），
    //         然后 workers_.reserve 并启动 opts_.num_workers 个线程跑 worker_loop
}

ThreadPool::~ThreadPool() {
    // TODO(4) 若尚未 shutdown 则 shutdown(true)；join 所有 worker（保证析构后无线程存活）
}

void ThreadPool::shutdown(bool drain) {
    (void)drain;
    // TODO(5) 关闭流程：
    //   1. { std::lock_guard<std::mutex> lk(mu_); closed_ = true; if (!drain) cancel_ = true; }
    //   2. cv_not_empty_.notify_all() 与 cv_not_full_.notify_all()
    //      （同时唤醒 worker 和阻塞在 kBlock 上的提交方）
    //   3. 阻塞中的提交方醒来发现 closed_ 后应抛 PoolClosedError——
    //      注意：此时它手里的任务没有入队，去哪了？把决策写进 README。
}

PoolStats ThreadPool::stats() const {
    PoolStats s;
    s.submitted = submitted_.load(std::memory_order_relaxed);
    s.completed = completed_.load(std::memory_order_relaxed);
    s.rejected  = rejected_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(mu_);
        s.queue_depth = static_cast<uint64_t>(tasks_.size());
    }
    return s;
}

void ThreadPool::worker_loop() {
    // TODO(6) 主循环：
    //   std::function<void()> task;
    //   while (true) {
    //     { std::unique_lock<std::mutex> lk(mu_);
    //       cv_not_empty_.wait(lk, [&]{ return !tasks_.empty() || closed_; });
    //       if (closed_ && (cancel_ || tasks_.empty())) return;
    //       task = std::move(tasks_.front()); tasks_.pop_front();
    //       if (队列从满变为非满) cv_not_full_.notify_one();   // 想想为什么要在这里通知
    //     }   // <- 解锁后再执行，缩小临界区
    //     try { task(); } catch (...) { /* post 路径吞异常并计数；future 路径异常已被 packaged_task 捕获 */ }
    //     completed_++;
    //   }
}

}  // namespace bpool
