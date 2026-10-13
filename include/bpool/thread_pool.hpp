// bounded-threadpool：有界队列线程池（单锁基线版）
// 接口契约见 README「规格 ↔ 测试对照」；设计决策（D1~D7）见 README「设计决策」节。
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
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace bpool {

// 队列满时的拒绝策略
enum class RejectPolicy {
    kBlock,       // submit/post 阻塞等待空位（背压）
    kThrow,       // 抛出 QueueFullError，rejected 计数 +1
    kCallerRuns,  // 在调用方线程同步执行该任务
};

struct PoolOptions {
    size_t num_workers   = std::thread::hardware_concurrency();
    size_t queue_capacity = 1024;  // 有界队列容量，必须 >= 1
    RejectPolicy policy  = RejectPolicy::kBlock;
};

class QueueFullError : public std::runtime_error {
public:
    explicit QueueFullError(const char* what) : std::runtime_error(what) {}
};

class PoolClosedError : public std::runtime_error {
public:
    explicit PoolClosedError(const char* what) : std::runtime_error(what) {}
};

struct PoolStats {
    uint64_t submitted   = 0;  // 成功入队（含 caller-runs 同步执行）
    uint64_t completed   = 0;  // 执行完毕
    uint64_t rejected    = 0;  // kThrow 拒绝次数
    uint64_t queue_depth = 0;  // 采样时的队列长度
};

class ThreadPool {
public:
    explicit ThreadPool(PoolOptions opts = {});
    ~ThreadPool();  // 等价于 shutdown(true)

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 轻量提交：无返回值、无 future 开销（基准测试与批量查询走这条路径）
    template <class F>
    void post(F&& f);

    // 带返回值提交：任务异常经 future 传递给调用方，worker 不受影响
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>;

    // drain=true : 停止接收新任务，排空队列后退出（不丢任务）
    // drain=false: 立即退出，丢弃未执行任务
    void shutdown(bool drain = true);

    PoolStats stats() const;

private:
    void worker_loop();

    PoolOptions opts_;

    mutable std::mutex mu_;
    std::condition_variable cv_not_empty_;  // 队列 空->非空，唤醒 worker
    std::condition_variable cv_not_full_;   // 队列 满->非满，唤醒阻塞中的 post(kBlock)
    std::condition_variable cv_shutdown_;   // worker 全部退出，唤醒等待排空的 shutdown(true)
    std::deque<std::function<void()>> tasks_;
    bool closed_ = false;      // 不再接受新任务
    bool cancel_ = false;      // drain=false 时置位，worker 立刻退出
    size_t active_workers_ = 0;  // 受 mu_ 保护：尚未退出 worker_loop 的线程数

    std::vector<std::thread> workers_;

    mutable std::atomic<uint64_t> submitted_{0};
    mutable std::atomic<uint64_t> completed_{0};
    mutable std::atomic<uint64_t> rejected_{0};
};

// ---------- 模板实现 ----------

template <class F>
void ThreadPool::post(F&& f) {
    std::unique_lock<std::mutex> lk(mu_);
    if (closed_) throw PoolClosedError("post on closed pool");
    if (tasks_.size() >= opts_.queue_capacity) {
        switch (opts_.policy) {
        case RejectPolicy::kThrow:
            rejected_.fetch_add(1, std::memory_order_relaxed);
            throw QueueFullError("queue full");
        case RejectPolicy::kBlock:
            // D1: 谓词含 closed_——shutdown 唤醒后重查，任务不入队不执行，抛 PoolClosedError
            cv_not_full_.wait(lk, [&] {
                return tasks_.size() < opts_.queue_capacity || closed_;
            });
            if (closed_) throw PoolClosedError("post on closed pool");
            break;
        case RejectPolicy::kCallerRuns:
            // D2: caller-runs 同步执行，submitted 与 completed 都计数
            lk.unlock();
            submitted_.fetch_add(1, std::memory_order_relaxed);
            std::forward<F>(f)();  // 异常自然透传给调用方（同步执行，无吞掉它的 worker）
            completed_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    tasks_.emplace_back(std::forward<F>(f));
    submitted_.fetch_add(1, std::memory_order_relaxed);
    cv_not_empty_.notify_one();
}

template <class F, class... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
    using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
    // packaged_task 只能移动，入 std::function 需套 shared_ptr；
    // 任务抛异常时被捕获存入 future，worker 调用点不感知
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
