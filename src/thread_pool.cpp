#include "bpool/thread_pool.hpp"

namespace bpool {

ThreadPool::ThreadPool(PoolOptions opts) : opts_(std::move(opts)) {
    if (opts_.num_workers == 0)
        throw std::invalid_argument("num_workers must be >= 1");
    if (opts_.queue_capacity == 0)
        throw std::invalid_argument("queue_capacity must be >= 1");
    workers_.reserve(opts_.num_workers);
    try {
        for (size_t i = 0; i < opts_.num_workers; ++i) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                ++active_workers_;  // 先计数再起线程，worker 退出时的减一才能配对
            }
            try {
                workers_.emplace_back([this] { worker_loop(); });
            } catch (...) {
                { std::lock_guard<std::mutex> lk(mu_); --active_workers_; }
                throw;
            }
        }
    } catch (...) {
        // 线程创建中途失败：关闸唤醒已启动的 worker 并 join，不让线程逃过析构
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
        throw;
    }
}

ThreadPool::~ThreadPool() {
    shutdown(true);
    for (auto& t : workers_)
        if (t.joinable()) t.join();
}

void ThreadPool::shutdown(bool drain) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = true;
        // D5: drain=false 立即清空队列——被丢弃任务的 packaged_task 就地销毁，
        // 对应 future 马上变为 broken_promise，而不是等到池析构才兑现
        if (!drain) {
            cancel_ = true;
            tasks_.clear();
        }
    }
    cv_not_empty_.notify_all();  // 唤醒 worker 重查 closed_
    cv_not_full_.notify_all();   // 唤醒阻塞在 kBlock 上的提交方（D1：醒来抛 PoolClosedError）
    if (drain) {
        // D7: drain=true 同步等待——返回即代表「已提交任务全部执行完毕、worker 已退出」；
        //     drain=false 不等（运行中的任务无法安全打断，交给析构 join）。
        //     注意：不得在任务体内调用本函数，否则自等死锁
        std::unique_lock<std::mutex> lk(mu_);
        cv_shutdown_.wait(lk, [&] { return active_workers_ == 0; });
    }
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
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_not_empty_.wait(lk, [&] { return !tasks_.empty() || closed_; });
            if (closed_ && (cancel_ || tasks_.empty())) break;  // 退出前先登记（见循环外）
            // D3: pop 后立刻让出空位通知（而非任务执行完之后），生产者尽早拿到空位；
            //    仅在队列原本满时通知——谓词等待保证多通知无害，但白付一次系统调用
            const bool was_full = tasks_.size() >= opts_.queue_capacity;
            task = std::move(tasks_.front());
            tasks_.pop_front();
            if (was_full) cv_not_full_.notify_one();
        }  // 解锁后再执行任务，临界区只覆盖队列操作
        try {
            task();  // post 路径异常在此吞掉；submit 路径异常已被 packaged_task 存入 future
        } catch (...) {
        }
        completed_.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        --active_workers_;
    }
    cv_shutdown_.notify_all();
}

}  // namespace bpool
