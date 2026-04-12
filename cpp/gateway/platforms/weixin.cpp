// Phase 12 — Weixin (WeChat Official Account) platform adapter implementation.
#include "weixin.hpp"

#include <regex>

namespace hermes::gateway::platforms {

WeixinAdapter::WeixinAdapter(Config cfg) : cfg_(std::move(cfg)) {}

bool WeixinAdapter::connect() {
    if (cfg_.appid.empty() || cfg_.appsecret.empty()) return false;
    // TODO(phase-14+): obtain access_token from Weixin API.
    return true;
}

void WeixinAdapter::disconnect() {}

bool WeixinAdapter::send(const std::string& /*chat_id*/,
                         const std::string& /*content*/) {
    return true;
}

void WeixinAdapter::send_typing(const std::string& /*chat_id*/) {}

namespace {
// Extract text between <Tag> and </Tag> from XML.
std::string extract_xml_tag(const std::string& xml, const std::string& tag) {
    // Try CDATA first: <Tag><![CDATA[value]]></Tag>
    std::string cdata_open = "<" + tag + "><![CDATA[";
    std::string cdata_close = "]]></" + tag + ">";
    auto pos = xml.find(cdata_open);
    if (pos != std::string::npos) {
        auto start = pos + cdata_open.size();
        auto end = xml.find(cdata_close, start);
        if (end != std::string::npos) return xml.substr(start, end - start);
    }
    // Fallback: <Tag>value</Tag>
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
