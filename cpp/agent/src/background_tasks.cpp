#include "hermes/agent/background_tasks.hpp"

#include <utility>

namespace hermes::agent {

BackgroundTaskPool::BackgroundTaskPool(std::size_t n_workers) {
    if (n_workers == 0) n_workers = 1;
    workers_.reserve(n_workers);
    for (std::size_t i = 0; i < n_workers; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

BackgroundTaskPool::~BackgroundTaskPool() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        shutdown_ = true;
    }
    have_work_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

void BackgroundTaskPool::submit(Task t) {
    if (!t) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return;  // reject after shutdown starts
        queue_.push(std::move(t));
    }
    have_work_.notify_one();
}

std::size_t BackgroundTaskPool::pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    return queue_.size();
}

void BackgroundTaskPool::wait_idle() {
    std::unique_lock<std::mutex> lk(mu_);
    now_idle_.wait(lk, [this] {
        return queue_.empty() && active_.load() == 0;
    });
}

void BackgroundTaskPool::worker_loop() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(mu_);
            have_work_.wait(lk, [this] {
                return shutdown_ || !queue_.empty();
            });
            if (shutdown_ && queue_.empty()) return;
            task = std::move(queue_.front());
            queue_.pop();
            active_.fetch_add(1);
        }
        try {
            task();
        } catch (...) {
            // Swallow — task-level failures must not tear down the pool.
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            active_.fetch_sub(1);
            if (queue_.empty() && active_.load() == 0) {
                now_idle_.notify_all();
            }
        }
    }
}

}  // namespace hermes::agent
