// Result delivery router — routes job output to platforms or local files.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hermes::cron {

struct DeliveryTarget {
    std::string platform;    // "telegram", "discord", "local", "origin"
    std::string chat_id;
    std::string thread_id;
    bool is_origin = false;
    bool is_explicit = false;
};

// Parse a delivery target string.
// "origin"               -> is_origin=true
// "local"                -> platform="local"
// "telegram"             -> platform="telegram" (home channel)
// "telegram:123456"      -> platform="telegram", chat_id="123456"
// "telegram:123456:789"  -> + thread_id="789"
DeliveryTarget parse_delivery_target(std::string_view target_str);

class DeliveryRouter {
public:
    // Phase 10: "local" writes to ~/.hermes/cron/output/{job_id}/{run_id}.txt
    // Platform targets return error (gateway not wired yet).
    void deliver(const std::string& content,
                 const std::vector<DeliveryTarget>& targets,
                 const std::string& job_id,
                 const std::string& run_id);

private:
    void deliver_local(const std::string& content,
                       const std::string& job_id,
                       const std::string& run_id);
};

}  // namespace hermes::cron
