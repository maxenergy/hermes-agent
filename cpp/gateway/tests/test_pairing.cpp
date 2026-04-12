#include <gtest/gtest.h>

#include <hermes/gateway/pairing.hpp>

#include <filesystem>
#include <set>

namespace hg = hermes::gateway;
namespace fs = std::filesystem;

class PairingStoreTest : public ::testing::Test {
protected:
    fs::path tmp_dir_;

    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("hermes_pairing_test_" +
                    std::to_string(std::chrono::system_clock::now()
                                       .time_since_epoch()
                                       .count()));
        fs::create_directories(tmp_dir_);
    }

    void TearDown() override { fs::remove_all(tmp_dir_); }
};

TEST_F(PairingStoreTest, GenerateCodeLength) {
    hg::PairingStore store(tmp_dir_);
    auto code = store.generate_code(hg::Platform::Telegram, "user1", "Alice");
    EXPECT_EQ(static_cast<int>(code.size()), hg::PairingStore::CODE_LENGTH);

    // All characters should be from the unambiguous alphabet.
    const std::string alphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    for (char c : code) {
        EXPECT_NE(alphabet.find(c), std::string::npos)
            << "Invalid char: " << c;
    }
}

TEST_F(PairingStoreTest, ApproveRoundTrip) {
    hg::PairingStore store(tmp_dir_);
    auto code =
        store.generate_code(hg::Platform::Telegram, "user1", "Alice");

    EXPECT_FALSE(store.is_approved(hg::Platform::Telegram, "user1"));

    bool ok = store.approve_code(hg::Platform::Telegram, code);
    EXPECT_TRUE(ok);

    EXPECT_TRUE(store.is_approved(hg::Platform::Telegram, "user1"));
}

TEST_F(PairingStoreTest, InvalidCodeRejected) {
    hg::PairingStore store(tmp_dir_);
    store.generate_code(hg::Platform::Telegram, "user1", "Alice");

    bool ok = store.approve_code(hg::Platform::Telegram, "BADCODE9");
    EXPECT_FALSE(ok);
    EXPECT_FALSE(store.is_approved(hg::Platform::Telegram, "user1"));
}

TEST_F(PairingStoreTest, MaxPendingEnforced) {
    hg::PairingStore store(tmp_dir_);

    std::set<std::string> codes;
    for (int i = 0; i < hg::PairingStore::MAX_PENDING + 2; ++i) {
        auto code = store.generate_code(
            hg::Platform::Telegram, "user" + std::to_string(i),
            "User" + std::to_string(i));
        codes.insert(code);
    }

    // All generated codes should be unique.
    EXPECT_EQ(codes.size(),
              static_cast<size_t>(hg::PairingStore::MAX_PENDING + 2));
}

TEST_F(PairingStoreTest, UniqueCodesGenerated) {
    hg::PairingStore store(tmp_dir_);
    std::set<std::string> codes;
    for (int i = 0; i < 10; ++i) {
        auto code = store.generate_code(
            hg::Platform::Discord, "user" + std::to_string(i),
            "User" + std::to_string(i));
        codes.insert(code);
    }
    // With 32^8 possible codes, collisions should not occur in 10 draws.
    EXPECT_EQ(codes.size(), 10u);
}

TEST_F(PairingStoreTest, CrossPlatformIsolation) {
    hg::PairingStore store(tmp_dir_);
    auto code1 =
        store.generate_code(hg::Platform::Telegram, "user1", "Alice");
    store.approve_code(hg::Platform::Telegram, code1);

    // Approved on Telegram, not on Discord.
    EXPECT_TRUE(store.is_approved(hg::Platform::Telegram, "user1"));
    EXPECT_FALSE(store.is_approved(hg::Platform::Discord, "user1"));
}
