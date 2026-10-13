// bounded-threadpool：有界队列线程池（单锁基线版）
// 接口契约见 README「规格 ↔ 测试对照」；代码内 TODO(n) 与根目录 README 的日程映射。
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
    std::deque<std::function<void()>> tasks_;
    bool closed_ = false;  // 不再接受新任务
    bool cancel_ = false;  // drain=false 时置位，worker 立刻退出

    std::vector<std::thread> workers_;

    mutable std::atomic<uint64_t> submitted_{0};
    mutable std::atomic<uint64_t> completed_{0};
    mutable std::atomic<uint64_t> rejected_{0};
};

// ---------- 模板实现（骨架阶段为 TODO 存根，实现后删除 throw） ----------

template <class F>
void ThreadPool::post(F&& f) {
    (void)f;
    // TODO(1) 基本提交路径：
    //   1. std::unique_lock<std::mutex> lk(mu_);
    //   2. 若 closed_ -> throw PoolClosedError("post on closed pool")
    //   3. 队列满（tasks_.size() >= opts_.queue_capacity）时按 opts_.policy 处理：
    //      kThrow     : rejected_++ 后 throw QueueFullError("queue full")
    //      kBlock     : cv_not_full_.wait(lk, [&]{ return tasks_.size() < cap || closed_; })
    //                   醒来后重查 closed_（应抛 PoolClosedError，想清楚语义并写进 README）
    //      kCallerRuns: 解锁后在调用方线程直接 f()；submitted_++（completed 是否也 ++？定好语义）
    //   4. 入队，submitted_++，cv_not_empty_.notify_one()
    throw std::logic_error("post: not implemented");
}

template <class F, class... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
    (void)f;
    (static_cast<void>(args), ...);
    // TODO(2) 在 post 的基础上包装 packaged_task：
    //   auto task = std::make_shared<std::packaged_task<R()>>(
    //       [f = std::forward<F>(f),
    //        tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
    //           return std::apply(std::move(f), std::move(tup));
    //       });
    //   auto fut = task->get_future();
    //   把 [task] { (*task)(); } 作为一个普通任务走 post 路径
    //   （任务抛异常时 packaged_task 会捕获并存入 future，worker 不受影响）
    //   return fut;
    throw std::logic_error("submit: not implemented");
}

}  // namespace bpool
