// BackgroundTaskPool — port of Python's asyncio.create_task pattern used by
// AIAgent._spawn_background_review for non-blocking side work
// (insights / trajectory review / memory GC).
//
// A fixed-size worker pool that drains a FIFO queue of std::function<void()>
// tasks.  Destruction signals all workers to stop, lets currently-running
// tasks finish, then joins the threads.  Submitted tasks that have not yet
// started at destruction time are discarded (matching Python's behaviour
// when the event loop exits with pending tasks — they are cancelled).
//
// Thread safety: submit(), pending(), wait_idle() are all thread-safe.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace hermes::agent {

class BackgroundTaskPool {
public:
    using Task = std::function<void()>;

    explicit BackgroundTaskPool(std::size_t n_workers = 2);
    ~BackgroundTaskPool();

    BackgroundTaskPool(const BackgroundTaskPool&) = delete;
    BackgroundTaskPool& operator=(const BackgroundTaskPool&) = delete;

    // Enqueue a task.  Returns immediately; task runs asynchronously on a
    // worker thread.  Tasks that throw have their exception swallowed
    // (logging is the caller's responsibility) — same as Python, where an
    // unhandled task exception is logged but doesn't tear down the loop.
    void submit(Task t);

    // Current queue depth (does not include running tasks).
    std::size_t pending() const;

    // Block until the queue is empty AND all workers are idle.
    void wait_idle();

    // Number of worker threads (set at construction).
    std::size_t worker_count() const { return workers_.size(); }

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    mutable std::mutex mu_;
    std::condition_variable have_work_;
    std::condition_variable now_idle_;
    std::queue<Task> queue_;
    std::atomic<std::size_t> active_{0};
    bool shutdown_ = false;
};

}  // namespace hermes::agent
