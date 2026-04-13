#include "hermes/agent/context_references.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace hermes::agent {

namespace {

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw std::runtime_error("context_references: cannot open " + p.string());
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

std::string clamp_content(std::string in) {
    if (in.size() > ContextReferences::kMaxContentBytes) {
        in.resize(ContextReferences::kMaxContentBytes);
        in.append("\n…[truncated]");
    }
    return in;
}

const char* kind_str(ContextRefKind k) {
    switch (k) {
        case ContextRefKind::File:    return "file";
        case ContextRefKind::Url:     return "url";
        case ContextRefKind::Snippet: return "snippet";
    }
    return "unknown";
}

}  // namespace

std::string ContextReferences::xml_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&':  out.append("&amp;");  break;
            case '<':  out.append("&lt;");   break;
            case '>':  out.append("&gt;");   break;
            case '"':  out.append("&quot;"); break;
            default:   out.push_back(c);     break;
        }
    }
    return out;
}

std::string ContextReferences::render_one(const ContextReference& r) {
    std::ostringstream os;
    os << "<context kind=\"" << kind_str(r.kind)
       << "\" ref=\"" << xml_escape(r.source) << "\"";
    if (!r.label.empty()) {
        os << " label=\"" << xml_escape(r.label) << "\"";
    }
    os << ">\n" << r.content;
    if (!r.content.empty() && r.content.back() != '\n') os << '\n';
    os << "</context>";
    return os.str();
}

const ContextReference& ContextReferences::register_file(
    const std::filesystem::path& path, bool stable) {
    auto content = clamp_content(read_file(path));
    std::lock_guard<std::mutex> lock(mu_);
    ContextReference r;
    r.kind = ContextRefKind::File;
    r.source = path.string();
    r.content = std::move(content);
    r.loaded_at = std::chrono::system_clock::now();
    r.stable = stable;
    r.label = path.filename().string();
    refs_.push_back(std::move(r));
    return refs_.back();
}

const ContextReference& ContextReferences::register_url(
    const std::string& url, std::string content, bool stable) {
    std::lock_guard<std::mutex> lock(mu_);
    ContextReference r;
    r.kind = ContextRefKind::Url;
    r.source = url;
    r.content = clamp_content(std::move(content));
    r.loaded_at = std::chrono::system_clock::now();
    r.stable = stable;
    refs_.push_back(std::move(r));
    return refs_.back();
}

const ContextReference& ContextReferences::register_snippet(
    const std::string& source, std::string content, bool stable) {
    std::lock_guard<std::mutex> lock(mu_);
    ContextReference r;
    r.kind = ContextRefKind::Snippet;
    r.source = source;
    r.content = clamp_content(std::move(content));
    r.loaded_at = std::chrono::system_clock::now();
    r.stable = stable;
    refs_.push_back(std::move(r));
    return refs_.back();
}

bool ContextReferences::remove(ContextRefKind kind, const std::string& source) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(refs_.begin(), refs_.end(), [&](const auto& r) {
        return r.kind == kind && r.source == source;
    });
    if (it == refs_.end()) return false;
    refs_.erase(it);
    return true;
}

void ContextReferences::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    refs_.clear();
}

std::vector<ContextReference> ContextReferences::list() const {
    std::lock_guard<std::mutex> lock(mu_);
    return refs_;
}

std::size_t ContextReferences::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return refs_.size();
}

bool ContextReferences::empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return refs_.empty();
}

std::string ContextReferences::render_stable_block() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::ostringstream os;
    bool any = false;
    for (const auto& r : refs_) {
        if (!r.stable) continue;
        if (!any) {
            os << "## Context references\n";
            any = true;
        } else {
            os << "\n";
        }
        os << render_one(r);
    }
    return any ? os.str() : std::string{};
}

std::string ContextReferences::drain_per_turn_block() {
    std::lock_guard<std::mutex> lock(mu_);
    std::ostringstream os;
    bool any = false;
    std::vector<ContextReference> kept;
    kept.reserve(refs_.size());
    for (auto& r : refs_) {
        if (r.stable) {
            kept.push_back(std::move(r));
            continue;
        }
        if (!any) {
            os << "The user attached the following references for this "
                  "turn:\n";
            any = true;
        } else {
            os << "\n";
        }
        os << render_one(r);
    }
    refs_ = std::move(kept);
    return any ? os.str() : std::string{};
}

}  // namespace hermes::agent
