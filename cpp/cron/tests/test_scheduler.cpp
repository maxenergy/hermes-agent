#include <hermes/cron/scheduler.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace hermes::cron;
namespace fs = std::filesystem;

class SchedulerTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / "hermes_sched_test";
        fs::remove_all(tmp_dir_);
        store_ = std::make_unique<JobStore>(tmp_dir_);
    }
    void TearDown() override {
        fs::remove_all(tmp_dir_);
    }

    Job make_job(bool due_now, bool paused = false) {
        Job j;
        j.name = "test";
        j.schedule_str = "* * * * *";  // every minute
        j.schedule = parse(j.schedule_str);
        j.prompt = "test prompt";
        j.delivery_targets = {"local"};
        j.paused = paused;
        j.created_at = std::chrono::system_clock::now();
        if (due_now) {
            // Set next_run to the past so it fires immediately.
            j.next_run = std::chrono::system_clock::now() -
                         std::chrono::seconds(60);
        } else {
            j.next_run = std::chrono::system_clock::now() +
                         std::chrono::hours(24);
        }
        return j;
    }

    fs::path tmp_dir_;
    std::unique_ptr<JobStore> store_;
};

TEST_F(SchedulerTest, StartStopLifecycle) {
    std::atomic<int> calls{0};
    Scheduler sched(store_.get(), [&](const Job&) {
        calls++;
        JobResult r;
        r.job_id = "x";
        r.run_id = "r";
        r.success = true;
        r.started_at = std::chrono::system_clock::now();
        r.finished_at = std::chrono::system_clock::now();
        return r;
    });
    EXPECT_FALSE(sched.running());
    sched.start();
    EXPECT_TRUE(sched.running());
    sched.stop();
    EXPECT_FALSE(sched.running());
}

TEST_F(SchedulerTest, DueJobExecutes) {
    auto job = make_job(true);
    auto id = store_->create(job);
    std::atomic<int> calls{0};
    Scheduler sched(store_.get(), [&](const Job& j) {
        calls++;
        JobResult r;
        r.job_id = j.id;
        r.run_id = "r1";
        r.output = "done";
        r.success = true;
        r.started_at = std::chrono::system_clock::now();
        r.finished_at = std::chrono::system_clock::now();
        return r;
    });
    sched.start();
    // Give the scheduler time to fire.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sched.stop();
    EXPECT_GE(calls.load(), 1);
    // Job should have updated run_count.
    auto updated = store_->get(id);
    ASSERT_TRUE(updated.has_value());
    EXPECT_GE(updated->run_count, 1);
}

TEST_F(SchedulerTest, PausedJobSkipped) {
    auto job = make_job(true, /*paused=*/true);
    store_->create(job);
    std::atomic<int> calls{0};
    Scheduler sched(store_.get(), [&](const Job&) {
        calls++;
        JobResult r;
        r.success = true;
        r.started_at = std::chrono::system_clock::now();
        r.finished_at = std::chrono::system_clock::now();
        return r;
    });
    sched.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sched.stop();
    EXPECT_EQ(calls.load(), 0);
}

TEST_F(SchedulerTest, ManualTrigger) {
    auto job = make_job(false);  // not due
    auto id = store_->create(job);
    std::atomic<int> calls{0};
    Scheduler sched(store_.get(), [&](const Job& j) {
        calls++;
        JobResult r;
        r.job_id = j.id;
        r.run_id = "manual";
        r.output = "triggered";
        r.success = true;
        r.started_at = std::chrono::system_clock::now();
        r.finished_at = std::chrono::system_clock::now();
        return r;
    });
    // Don't need to start the scheduler loop for manual trigger.
    sched.trigger(id);
    EXPECT_EQ(calls.load(), 1);
    auto results = store_->get_results(id);
    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].run_id, "manual");
}
