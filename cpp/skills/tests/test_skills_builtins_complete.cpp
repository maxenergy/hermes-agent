// Parity check: the built-in skill collection shipped inside the C++ port
// (cpp/skills/builtins/) must enumerate the same SKILL.md files as the
// upstream Python skills/ tree. The test points the skill index at the
// built-in directory via HERMES_BUILTINS_DIR and verifies the count plus
// a few representative names.
//
// Compile-time defines (set by CMakeLists.txt):
//   HERMES_BUILTINS_SOURCE_DIR — path to cpp/skills/builtins/
//   HERMES_PYTHON_SKILLS_DIR   — path to Python skills/ tree (may not
//                                exist in minimal checkouts; optional)

#include "hermes/skills/skill_utils.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace hermes::skills;

namespace {

size_t count_skill_md(const fs::path& root) {
    size_t n = 0;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return 0;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && it->path().filename() == "SKILL.md") {
            ++n;
        }
    }
    return n;
}

// Environment override helper — mutates process env for the duration of
// a single test. Restores the prior value in the destructor.
class ScopedEnv {
public:
    ScopedEnv(const char* key, const std::string& value) : key_(key) {
        if (const char* prev = std::getenv(key)) {
            prev_value_ = prev;
            had_prev_ = true;
        }
        setenv(key_, value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (had_prev_) {
            setenv(key_, prev_value_.c_str(), 1);
        } else {
            unsetenv(key_);
        }
    }
private:
    const char* key_;
    std::string prev_value_;
    bool had_prev_ = false;
};

}  // namespace

TEST(SkillsBuiltinsCompleteTest, BuiltinDirectoryExists) {
    fs::path builtins = HERMES_BUILTINS_SOURCE_DIR;
    ASSERT_TRUE(fs::is_directory(builtins))
        << "cpp/skills/builtins/ missing — did you run the copy step?";
}

TEST(SkillsBuiltinsCompleteTest, BuiltinCountMatchesExpected) {
    fs::path builtins = HERMES_BUILTINS_SOURCE_DIR;
    // Expected count: 78 SKILL.md files at the time of the initial copy
    // from the Python tree. If the upstream skills collection grows, bump
    // this number after re-running the copy step.
    constexpr size_t kExpectedBuiltinCount = 78;
    EXPECT_EQ(count_skill_md(builtins), kExpectedBuiltinCount);
}

#ifdef HERMES_PYTHON_SKILLS_DIR
TEST(SkillsBuiltinsCompleteTest, BuiltinCountMatchesPythonTree) {
    fs::path py = HERMES_PYTHON_SKILLS_DIR;
    if (!fs::is_directory(py)) {
        GTEST_SKIP() << "Python skills/ tree not present (minimal checkout)";
    }
    fs::path builtins = HERMES_BUILTINS_SOURCE_DIR;
    EXPECT_EQ(count_skill_md(builtins), count_skill_md(py))
        << "Built-in skill collection is out of sync with Python tree.";
}
#endif

TEST(SkillsBuiltinsCompleteTest, IterSkillIndexDiscoversBuiltins) {
    ScopedEnv builtins_override("HERMES_BUILTINS_DIR", HERMES_BUILTINS_SOURCE_DIR);
    // Point HERMES_HOME at an empty temp dir so the test does not pick up
    // whatever the developer has installed in ~/.hermes/.
    char tpl[] = "/tmp/hermes_skills_parity_XXXXXX";
    fs::path tmp = fs::path(mkdtemp(tpl));
    ScopedEnv home_override("HERMES_HOME", tmp.string());

    auto skills = iter_skill_index();

    std::error_code ec;
    fs::remove_all(tmp, ec);

    // A representative subset — every layout variant must be discovered.
    std::set<std::string> names;
    for (const auto& s : skills) names.insert(s.name);

    // Layout A (flat): dogfood/SKILL.md
    EXPECT_TRUE(names.count("dogfood")) << "flat-layout skill missing";
    // Layout B (category): github/github-auth/SKILL.md
    EXPECT_TRUE(names.count("github/github-auth"))
        << "category-layout skill missing";
    // Layout C (nested category): mlops/inference/vllm/SKILL.md
    EXPECT_TRUE(names.count("mlops/inference/vllm"))
        << "nested-category-layout skill missing — iter_skill_index needs "
           "to walk 3 levels.";

    // Overall count lower bound: we should find at least the 78 builtins.
    EXPECT_GE(skills.size(), 78u);
}
