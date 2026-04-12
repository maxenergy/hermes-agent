// Background scheduler — fires agent jobs on cron schedules.
#pragma once

#include <hermes/cron/jobs.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace hermes::cron {

class Scheduler {
public:
    using ExecuteFn = std::function<JobResult(const Job& job)>;

    Scheduler(JobStore* store, ExecuteFn execute_fn);
    ~Scheduler();

    void start();   // spawns background thread
    void stop();    // signals stop + joins thread
    bool running() const;

    // Manual trigger (bypass schedule).
    void trigger(const std::string& job_id);

private:
    void run_loop();
    void execute_job(Job& job);

    JobStore* store_;
    ExecuteFn execute_fn_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::condition_variable cv_;
    std::mutex mu_;
};

}  // namespace hermes::cron
