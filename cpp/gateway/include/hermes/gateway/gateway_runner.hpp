// Main gateway orchestrator (skeleton).
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <hermes/gateway/gateway_config.hpp>
#include <hermes/gateway/hooks.hpp>
#include <hermes/gateway/session_store.hpp>

namespace hermes::gateway {

class BasePlatformAdapter {
public:
    virtual ~BasePlatformAdapter() = default;
    virtual Platform platform() const = 0;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool send(const std::string& chat_id,
                      const std::string& content) = 0;
    virtual void send_typing(const std::string& /*chat_id*/) {}
    // Phase 12 adds: send_image, send_voice, send_document, edit_message
};

struct MessageEvent {
    std::string text;
    std::string message_type;  // TEXT|PHOTO|VIDEO|AUDIO|VOICE|DOCUMENT|STICKER
    SessionSource source;
    std::vector<std::string> media_urls;
    std::optional<std::string> reply_to_message_id;
};

class GatewayRunner {
public:
    GatewayRunner(GatewayConfig config, SessionStore* sessions,
                  HookRegistry* hooks);

    void register_adapter(std::unique_ptr<BasePlatformAdapter> adapter);
    void start();  // connect all enabled adapters, emit gateway:startup
    void stop();   // disconnect all, write status

    void handle_message(const MessageEvent& event);

private:
    GatewayConfig config_;
    SessionStore* sessions_;
    HookRegistry* hooks_;
    std::vector<std::unique_ptr<BasePlatformAdapter>> adapters_;
    // Phase 12+ will add: agent cache, interrupt logic, command dispatch
};

}  // namespace hermes::gateway
