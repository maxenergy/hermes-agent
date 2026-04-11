#include "hermes/config/optional_env_vars.hpp"

namespace hermes::config {

namespace {

std::map<std::string, EnvVarSpec> build_map() {
    std::map<std::string, EnvVarSpec> m;

    auto add = [&](std::string name, std::string desc, std::string prompt,
                   std::string url, std::string category, bool password) {
        m.emplace(std::move(name),
                  EnvVarSpec{std::move(desc), std::move(prompt),
                             std::move(url), std::move(category), password});
    };

    // ── Providers ────────────────────────────────────────────────────
    add("OPENROUTER_API_KEY",
        "OpenRouter API key (for vision, web scraping helpers, and MoA)",
        "OpenRouter API key", "https://openrouter.ai/keys", "provider", true);
    add("ANTHROPIC_API_KEY",
        "Anthropic API key for Claude models",
        "Anthropic API key", "https://console.anthropic.com/", "provider", true);
    add("OPENAI_API_KEY",
        "OpenAI API key for GPT models",
        "OpenAI API key", "https://platform.openai.com/api-keys", "provider", true);
    add("GOOGLE_API_KEY",
        "Google AI Studio API key (also recognized as GEMINI_API_KEY)",
        "Google AI Studio API key", "https://aistudio.google.com/app/apikey",
        "provider", true);
    add("GEMINI_API_KEY",
        "Google AI Studio API key (alias for GOOGLE_API_KEY)",
        "Gemini API key", "https://aistudio.google.com/app/apikey",
        "provider", true);
    add("MISTRAL_API_KEY",
        "Mistral API key for Voxtral TTS and transcription (STT)",
        "Mistral API key", "https://console.mistral.ai/", "provider", true);
    add("MINIMAX_API_KEY",
        "MiniMax API key (international)",
        "MiniMax API key", "https://www.minimax.io/", "provider", true);
    add("GROQ_API_KEY",
        "Groq API key for low-latency inference",
        "Groq API key", "https://console.groq.com/keys", "provider", true);
    add("NOUS_BASE_URL",
        "Nous Portal base URL override",
        "Nous Portal base URL (leave empty for default)", "", "provider", false);
    add("DEEPSEEK_API_KEY",
        "DeepSeek API key for direct DeepSeek access",
        "DeepSeek API Key", "https://platform.deepseek.com/api_keys",
        "provider", true);

    // ── Tool APIs ────────────────────────────────────────────────────
    add("FIRECRAWL_API_KEY",
        "Firecrawl API key for web search and scraping",
        "Firecrawl API key", "https://firecrawl.dev/", "tool", true);
    add("EXA_API_KEY",
        "Exa API key for AI-native web search and contents",
        "Exa API key", "https://exa.ai/", "tool", true);
    add("PARALLEL_API_KEY",
        "Parallel API key for AI-native web search and extract",
        "Parallel API key", "https://parallel.ai/", "tool", true);
    add("TAVILY_API_KEY",
        "Tavily API key for AI-native web search, extract, and crawl",
        "Tavily API key", "https://app.tavily.com/home", "tool", true);
    add("ELEVENLABS_API_KEY",
        "ElevenLabs API key for premium text-to-speech voices",
        "ElevenLabs API key", "https://elevenlabs.io/", "tool", true);
    add("FAL_KEY",
        "FAL API key for image generation",
        "FAL API key", "https://fal.ai/", "tool", true);

    // ── Messaging platforms ─────────────────────────────────────────
    add("TELEGRAM_BOT_TOKEN",
        "Telegram bot token from @BotFather",
        "Telegram bot token", "https://t.me/BotFather", "messaging", true);
    add("TELEGRAM_ALLOWED_USERS",
        "Comma-separated Telegram user IDs allowed to use the bot",
        "Allowed Telegram user IDs (comma-separated)",
        "https://t.me/userinfobot", "messaging", false);
    add("DISCORD_BOT_TOKEN",
        "Discord bot token from Developer Portal",
        "Discord bot token", "https://discord.com/developers/applications",
        "messaging", true);
    add("SLACK_BOT_TOKEN",
        "Slack bot token (xoxb-) from OAuth & Permissions",
        "Slack Bot Token (xoxb-...)", "https://api.slack.com/apps",
        "messaging", true);
    add("SLACK_APP_TOKEN",
        "Slack app-level token (xapp-) for Socket Mode",
        "Slack App Token (xapp-...)", "https://api.slack.com/apps",
        "messaging", true);
    add("WHATSAPP_ENABLED",
        "Enable the WhatsApp platform gateway (true/false)",
        "Enable WhatsApp (true/false)", "", "messaging", false);
    add("MATRIX_HOMESERVER",
        "Matrix homeserver URL (e.g. https://matrix.example.org)",
        "Matrix homeserver URL", "https://matrix.org/ecosystem/servers/",
        "messaging", false);
    add("MATRIX_ACCESS_TOKEN",
        "Matrix access token for the bot account",
        "Matrix access token", "", "messaging", true);

    return m;
}

}  // namespace

const std::map<std::string, EnvVarSpec>& optional_env_vars() {
    static const std::map<std::string, EnvVarSpec> kVars = build_map();
    return kVars;
}

}  // namespace hermes::config
