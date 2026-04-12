// Phase 12 — Weixin (WeChat Official Account) platform adapter implementation.
#include "weixin.hpp"

#include <nlohmann/json.hpp>

namespace hermes::gateway::platforms {

WeixinAdapter::WeixinAdapter(Config cfg) : cfg_(std::move(cfg)) {}

WeixinAdapter::WeixinAdapter(Config cfg, hermes::llm::HttpTransport* transport)
    : cfg_(std::move(cfg)), transport_(transport) {}

hermes::llm::HttpTransport* WeixinAdapter::get_transport() {
    if (transport_) return transport_;
    return hermes::llm::get_default_transport();
}

bool WeixinAdapter::connect() {
    if (cfg_.appid.empty() || cfg_.appsecret.empty()) return false;

    auto* transport = get_transport();
    if (!transport) return false;

    // Obtain access_token from Weixin API.
    try {
        std::string url = "https://api.weixin.qq.com/cgi-bin/token"
                          "?grant_type=client_credential"
                          "&appid=" + cfg_.appid +
                          "&secret=" + cfg_.appsecret;
        auto resp = transport->get(url, {});
        if (resp.status_code != 200) return false;

        auto body = nlohmann::json::parse(resp.body);
        if (!body.contains("access_token")) return false;
        wx_access_token_ = body["access_token"].get<std::string>();
        return true;
    } catch (...) {
        return false;
    }
}

void WeixinAdapter::disconnect() {
    wx_access_token_.clear();
}

bool WeixinAdapter::send(const std::string& chat_id,
                         const std::string& content) {
    auto* transport = get_transport();
    if (!transport) return false;

    // Weixin customer service message API.
    nlohmann::json payload = {
        {"touser", chat_id},
        {"msgtype", "text"},
        {"text", {{"content", content}}}
    };

    try {
        auto resp = transport->post_json(
            "https://api.weixin.qq.com/cgi-bin/message/custom/send"
            "?access_token=" + wx_access_token_,
            {{"Content-Type", "application/json"}},
            payload.dump());
        if (resp.status_code != 200) return false;

        auto body = nlohmann::json::parse(resp.body);
        return body.value("errcode", -1) == 0;
    } catch (...) {
        return false;
    }
}

void WeixinAdapter::send_typing(const std::string& chat_id) {
    auto* transport = get_transport();
    if (!transport) return;

    nlohmann::json payload = {
        {"touser", chat_id},
        {"command", "Typing"}
    };

    try {
        transport->post_json(
            "https://api.weixin.qq.com/cgi-bin/message/custom/typing"
            "?access_token=" + wx_access_token_,
            {{"Content-Type", "application/json"}},
            payload.dump());
    } catch (...) {
        // Best-effort.
    }
}

namespace {
std::string extract_xml_tag(const std::string& xml, const std::string& tag) {
    std::string cdata_open = "<" + tag + "><![CDATA[";
    std::string cdata_close = "]]></" + tag + ">";
    auto pos = xml.find(cdata_open);
    if (pos != std::string::npos) {
        auto start = pos + cdata_open.size();
        auto end = xml.find(cdata_close, start);
        if (end != std::string::npos) return xml.substr(start, end - start);
    }
    std::string open = "<" + tag + ">";
    std::string close = "</" + tag + ">";
    pos = xml.find(open);
    if (pos != std::string::npos) {
        auto start = pos + open.size();
        auto end = xml.find(close, start);
        if (end != std::string::npos) return xml.substr(start, end - start);
    }
    return {};
}
}  // namespace

WeixinMessageEvent WeixinAdapter::parse_xml_message(const std::string& xml) {
    WeixinMessageEvent evt;
    evt.from_user = extract_xml_tag(xml, "FromUserName");
    evt.to_user = extract_xml_tag(xml, "ToUserName");
    evt.msg_type = extract_xml_tag(xml, "MsgType");
    evt.content = extract_xml_tag(xml, "Content");
    evt.msg_id = extract_xml_tag(xml, "MsgId");
    return evt;
}

}  // namespace hermes::gateway::platforms
