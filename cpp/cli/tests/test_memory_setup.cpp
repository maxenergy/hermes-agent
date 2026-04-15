// Tests for the C++17 port of `hermes_cli/memory_setup.py`.

#include <gtest/gtest.h>

#include <functional>
#include <unordered_set>

#include "hermes/cli/memory_setup.hpp"

using namespace hermes::cli::memory_setup;

TEST(MemorySetup, SetupHintLabels) {
    EXPECT_EQ(setup_hint_label(setup_hint::requires_api_key),
              "requires API key");
    EXPECT_EQ(setup_hint_label(setup_hint::api_key_or_local),
              "API key / local");
    EXPECT_EQ(setup_hint_label(setup_hint::no_setup_needed),
              "no setup needed");
    EXPECT_EQ(setup_hint_label(setup_hint::local), "local");
}

TEST(MemorySetup, ClassifyEmptyScheme) {
    EXPECT_EQ(classify_setup_hint({}), setup_hint::no_setup_needed);
}

TEST(MemorySetup, ClassifyOnlySecrets) {
    std::vector<schema_field> schema{schema_field{true, "api_key", "API_KEY"}};
    EXPECT_EQ(classify_setup_hint(schema), setup_hint::requires_api_key);
}

TEST(MemorySetup, ClassifyOnlyNonSecrets) {
    std::vector<schema_field> schema{schema_field{false, "host", "HOST"}};
    EXPECT_EQ(classify_setup_hint(schema), setup_hint::local);
}

TEST(MemorySetup, ClassifyMixed) {
    std::vector<schema_field> schema{
        schema_field{true, "api_key", "API_KEY"},
        schema_field{false, "host", "HOST"},
    };
    EXPECT_EQ(classify_setup_hint(schema), setup_hint::api_key_or_local);
}

TEST(MemorySetup, PipToImportOverrides) {
    EXPECT_EQ(pip_to_import_name("honcho-ai"), "honcho");
    EXPECT_EQ(pip_to_import_name("mem0ai"), "mem0");
    EXPECT_EQ(pip_to_import_name("hindsight-client"), "hindsight_client");
    EXPECT_EQ(pip_to_import_name("hindsight-all"), "hindsight");
}

TEST(MemorySetup, PipToImportFallbackReplacesDashes) {
    EXPECT_EQ(pip_to_import_name("some-package"), "some_package");
    EXPECT_EQ(pip_to_import_name("already_underscored"), "already_underscored");
    EXPECT_EQ(pip_to_import_name("simple"), "simple");
}

TEST(MemorySetup, PipToImportStripsExtras) {
    EXPECT_EQ(pip_to_import_name("package[extra]"), "package");
    EXPECT_EQ(pip_to_import_name("pkg-with-dash[extra1,extra2]"),
              "pkg_with_dash");
}

TEST(MemorySetup, MissingDepsEmptyWhenAllInstalled) {
    std::vector<std::string> deps{"honcho-ai", "mem0ai"};
    auto is_installed = [](const std::string&) { return true; };
    EXPECT_TRUE(
        compute_missing_pip_dependencies(deps, is_installed).empty());
}

TEST(MemorySetup, MissingDepsPreservesOrder) {
    std::vector<std::string> deps{"honcho-ai", "mem0ai", "hindsight-client"};
    std::unordered_set<std::string> installed{"honcho"};
    auto is_installed = [&](const std::string& name) {
        return installed.count(name) > 0;
    };
    auto missing = compute_missing_pip_dependencies(deps, is_installed);
    ASSERT_EQ(missing.size(), 2u);
    EXPECT_EQ(missing[0], "mem0ai");
    EXPECT_EQ(missing[1], "hindsight-client");
}

TEST(MemorySetup, MissingDepsWithoutCallback) {
    std::vector<std::string> deps{"mem0ai"};
    auto missing = compute_missing_pip_dependencies(deps, {});
    EXPECT_EQ(missing.size(), 1u);
}

TEST(MemorySetup, MaskExistingSecretShortValue) {
    EXPECT_EQ(mask_existing_secret(""), "set");
    EXPECT_EQ(mask_existing_secret("abc"), "set");
    EXPECT_EQ(mask_existing_secret("abcd"), "set");
}

TEST(MemorySetup, MaskExistingSecretLongValue) {
    EXPECT_EQ(mask_existing_secret("abcde"), "...bcde");
    EXPECT_EQ(mask_existing_secret("sk-thisIsLong"), "...Long");
}

TEST(MemorySetup, RenderEnvEmptyInputAddsKeys) {
    std::string out = render_env_file_update(
        "", {{"FOO", "bar"}, {"BAZ", "qux"}});
    EXPECT_EQ(out, "FOO=bar\nBAZ=qux\n");
}

TEST(MemorySetup, RenderEnvPreservesExisting) {
    std::string existing{"EXISTING=keep\nOTHER=also"};
    std::string out = render_env_file_update(existing, {{"NEW", "added"}});
    EXPECT_EQ(out, "EXISTING=keep\nOTHER=also\nNEW=added\n");
}

TEST(MemorySetup, RenderEnvReplacesExistingKey) {
    std::string existing{"FOO=old\nBAR=keep"};
    std::string out = render_env_file_update(existing, {{"FOO", "new"}});
    EXPECT_EQ(out, "FOO=new\nBAR=keep\n");
}

TEST(MemorySetup, RenderEnvHandlesMixedUpdate) {
    std::string existing{"KEEP=1\nREPLACE=old"};
    std::string out = render_env_file_update(
        existing, {{"REPLACE", "new"}, {"APPEND", "added"}});
    EXPECT_EQ(out, "KEEP=1\nREPLACE=new\nAPPEND=added\n");
}

TEST(MemorySetup, RenderEnvTrimsKeyWhitespace) {
    std::string existing{"  FOO  =old"};
    std::string out = render_env_file_update(existing, {{"FOO", "new"}});
    EXPECT_EQ(out, "FOO=new\n");
}

TEST(MemorySetup, RenderEnvSkipsCommentsWithoutEquals) {
    std::string existing{"# comment\nOTHER=kept"};
    std::string out = render_env_file_update(existing, {{"FOO", "bar"}});
    EXPECT_EQ(out, "# comment\nOTHER=kept\nFOO=bar\n");
}

TEST(MemorySetup, FormatProviderConfigLine) {
    EXPECT_EQ(format_provider_config_line("host", "localhost"),
              "    host: localhost");
}

TEST(MemorySetup, StatusLinesNoProviders) {
    status_context ctx{};
    auto lines = render_provider_status_lines(ctx);
    ASSERT_GE(lines.size(), 5u);
    EXPECT_EQ(lines[1], "Memory status");
    EXPECT_EQ(lines[3], "  Built-in:  always active");
    EXPECT_NE(lines[4].find("(none -- built-in only)"), std::string::npos);
}

TEST(MemorySetup, StatusLinesActiveProviderAvailable) {
    status_context ctx{};
    ctx.active_provider = "mem0";
    ctx.providers.push_back(
        discovered_provider{"mem0", "requires API key", true, {}});
    auto lines = render_provider_status_lines(ctx);
    bool saw_available{false};
    bool saw_installed{false};
    for (const auto& line : lines) {
        if (line.find("Status:    available") != std::string::npos) {
            saw_available = true;
        }
        if (line.find("Plugin:    installed") != std::string::npos) {
            saw_installed = true;
        }
    }
    EXPECT_TRUE(saw_available);
    EXPECT_TRUE(saw_installed);
}

TEST(MemorySetup, StatusLinesActiveProviderMissing) {
    status_context ctx{};
    ctx.active_provider = "mem0";
    ctx.providers.push_back(
        discovered_provider{"honcho", "local", true, {}});
    auto lines = render_provider_status_lines(ctx);
    bool saw_not_installed{false};
    for (const auto& line : lines) {
        if (line.find("NOT installed") != std::string::npos) {
            saw_not_installed = true;
        }
    }
    EXPECT_TRUE(saw_not_installed);
}

TEST(MemorySetup, StatusLinesActiveProviderUnavailableShowsMissingEnv) {
    status_context ctx{};
    ctx.active_provider = "mem0";
    ctx.providers.push_back(discovered_provider{
        "mem0", "requires API key", false,
        {schema_field{true, "api_key", "MEM0_API_KEY"}}});
    ctx.env_is_set = [](const std::string&) { return false; };
    auto lines = render_provider_status_lines(ctx);
    bool saw_missing_header{false};
    bool saw_env_line{false};
    for (const auto& line : lines) {
        if (line == "  Missing:") {
            saw_missing_header = true;
        }
        if (line.find("MEM0_API_KEY") != std::string::npos) {
            saw_env_line = true;
        }
    }
    EXPECT_TRUE(saw_missing_header);
    EXPECT_TRUE(saw_env_line);
}

TEST(MemorySetup, StatusLinesIncludesInstalledPlugins) {
    status_context ctx{};
    ctx.providers.push_back(
        discovered_provider{"mem0", "requires API key", true, {}});
    ctx.providers.push_back(
        discovered_provider{"honcho", "local", true, {}});
    ctx.active_provider = "mem0";
    auto lines = render_provider_status_lines(ctx);
    bool saw_active_mark{false};
    bool saw_honcho{false};
    for (const auto& line : lines) {
        if (line.find("mem0") != std::string::npos &&
            line.find("<- active") != std::string::npos) {
            saw_active_mark = true;
        }
        if (line.find("honcho") != std::string::npos) {
            saw_honcho = true;
        }
    }
    EXPECT_TRUE(saw_active_mark);
    EXPECT_TRUE(saw_honcho);
}

TEST(MemorySetup, StatusLinesRendersActiveProviderConfig) {
    status_context ctx{};
    ctx.active_provider = "mem0";
    ctx.active_provider_config = {{"host", "localhost"}, {"port", "6333"}};
    ctx.providers.push_back(
        discovered_provider{"mem0", "requires API key", true, {}});
    auto lines = render_provider_status_lines(ctx);
    bool saw_host{false};
    bool saw_port{false};
    for (const auto& line : lines) {
        if (line == "    host: localhost") {
            saw_host = true;
        }
        if (line == "    port: 6333") {
            saw_port = true;
        }
    }
    EXPECT_TRUE(saw_host);
    EXPECT_TRUE(saw_port);
}

TEST(MemorySetup, PipImportOverridesTableMatchesPython) {
    const auto& table = pip_import_overrides();
    EXPECT_EQ(table.size(), 4u);
    EXPECT_EQ(table.at("honcho-ai"), "honcho");
    EXPECT_EQ(table.at("mem0ai"), "mem0");
    EXPECT_EQ(table.at("hindsight-client"), "hindsight_client");
    EXPECT_EQ(table.at("hindsight-all"), "hindsight");
}
