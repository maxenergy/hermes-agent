#include <hermes/cron/scheduler.hpp>
#include <hermes/core/time.hpp>

#include <chrono>

namespace hermes::cron {

Scheduler::Scheduler(JobStore* store, ExecuteFn execute_fn)
    : store_(store), execute_fn_(std::move(execute_fn)) {}

Scheduler::~Scheduler() {
    stop();
}

void Scheduler::start() {
    if (running_.exchange(true)) return;  // Already running.
    thread_ = std::thread([this] { run_loop(); });
}

void Scheduler::stop() {
    if (!running_.exchange(false)) return;  // Already stopped.
    {
        std::lock_guard<std::mutex> lk(mu_);
        cv_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
}

bool Scheduler::running() const {
    return running_.load();
}

void Scheduler::trigger(const std::string& job_id) {
    auto job_opt = store_->get(job_id);
    if (!job_opt) return;
    auto job = *job_opt;
    execute_job(job);
}

void Scheduler::run_loop() {
    while (running_.load()) {
        auto now = hermes::core::time::now();
        auto jobs = store_->list_all();
        for (auto& job : jobs) {
            if (job.paused) continue;
            if (now >= job.next_run &&
                job.next_run.time_since_epoch().count() > 0) {
                execute_job(job);
            }
        }
        // Wait 30 seconds or until signalled.
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::seconds(30),
                     [this] { return !running_.load(); });
    }
}

void Scheduler::execute_job(Job& job) {
    auto result = execute_fn_(job);
    store_->save_result(result);

    job.last_run = hermes::core::time::now();
    job.run_count++;
    if (!result.success) job.fail_count++;
    job.next_run = next_fire(job.schedule, job.last_run);
    store_->update(job);
}

}  // namespace hermes::cron
