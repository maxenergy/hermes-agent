#include <hermes/cron/jobs.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>

using namespace hermes::cron;
namespace fs = std::filesystem;

class JobStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() / "hermes_cron_test";
        fs::remove_all(tmp_dir_);
        store_ = std::make_unique<JobStore>(tmp_dir_);
    }
    void TearDown() override {
        fs::remove_all(tmp_dir_);
    }

    Job make_job(const std::string& name = "test-job") {
        Job j;
        j.name = name;
        j.schedule_str = "*/5 * * * *";
        j.schedule = parse(j.schedule_str);
        j.prompt = "do something";
        j.model = "gpt-4";
        j.delivery_targets = {"local"};
        j.created_at = std::chrono::system_clock::now();
        j.next_run = next_fire(j.schedule, j.created_at);
        return j;
    }

    fs::path tmp_dir_;
    std::unique_ptr<JobStore> store_;
};

TEST_F(JobStoreTest, CreateAndGet) {
    auto job = make_job();
    auto id = store_->create(job);
    EXPECT_FALSE(id.empty());

    auto got = store_->get(id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "test-job");
    EXPECT_EQ(got->schedule_str, "*/5 * * * *");
    EXPECT_EQ(got->prompt, "do something");
    EXPECT_EQ(got->model, "gpt-4");
    EXPECT_EQ(got->delivery_targets.size(), 1u);
    EXPECT_EQ(got->delivery_targets[0], "local");
}

TEST_F(JobStoreTest, ListAll) {
    store_->create(make_job("job-a"));
    store_->create(make_job("job-b"));
    auto all = store_->list_all();
    EXPECT_EQ(all.size(), 2u);
}

TEST_F(JobStoreTest, Update) {
    auto job = make_job();
    auto id = store_->create(job);
    auto got = store_->get(id);
    ASSERT_TRUE(got.has_value());
    got->name = "updated-name";
    got->paused = true;
    store_->update(*got);

    auto got2 = store_->get(id);
    ASSERT_TRUE(got2.has_value());
    EXPECT_EQ(got2->name, "updated-name");
    EXPECT_TRUE(got2->paused);
}

TEST_F(JobStoreTest, Remove) {
    auto job = make_job();
    auto id = store_->create(job);
    EXPECT_TRUE(store_->get(id).has_value());
    store_->remove(id);
    EXPECT_FALSE(store_->get(id).has_value());
}

TEST_F(JobStoreTest, SaveAndGetResults) {
    auto job = make_job();
    auto id = store_->create(job);

    JobResult r;
    r.job_id = id;
    r.run_id = "run-001";
    r.output = "hello world";
    r.success = true;
    r.started_at = std::chrono::system_clock::now();
    r.finished_at = std::chrono::system_clock::now();
    store_->save_result(r);

    r.run_id = "run-002";
    r.success = false;
    r.output = "failed";
    store_->save_result(r);

    auto results = store_->get_results(id, 10);
    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].run_id, "run-001");
    EXPECT_TRUE(results[0].success);
    EXPECT_EQ(results[1].run_id, "run-002");
    EXPECT_FALSE(results[1].success);
}

TEST_F(JobStoreTest, GetNonExistent) {
    auto got = store_->get("nonexistent-id");
    EXPECT_FALSE(got.has_value());
}
