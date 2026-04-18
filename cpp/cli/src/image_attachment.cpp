#include "hermes/cli/image_attachment.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace hermes::cli {

namespace {

// Inline base64 implementation — small, no extra link dep.  We deliberately
// avoid pulling in OpenSSL's EVP_EncodeBlock here so the helper can be
// exercised in isolation by unit tests without tripping over missing
// symbols at link time on minimal builds.
constexpr char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::uint8_t* data, std::size_t n) {
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= n) {
        std::uint32_t w = (std::uint32_t(data[i])     << 16)
                        | (std::uint32_t(data[i + 1]) << 8)
                        |  std::uint32_t(data[i + 2]);
        out.push_back(kB64Alphabet[(w >> 18) & 0x3F]);
        out.push_back(kB64Alphabet[(w >> 12) & 0x3F]);
        out.push_back(kB64Alphabet[(w >> 6)  & 0x3F]);
        out.push_back(kB64Alphabet[ w        & 0x3F]);
        i += 3;
    }
    if (i < n) {
        std::uint32_t w = std::uint32_t(data[i]) << 16;
        if (i + 1 < n) w |= std::uint32_t(data[i + 1]) << 8;
        out.push_back(kB64Alphabet[(w >> 18) & 0x3F]);
        out.push_back(kB64Alphabet[(w >> 12) & 0x3F]);
        out.push_back(i + 1 < n ? kB64Alphabet[(w >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::string to_lower_ascii(std::string_view in) {
    std::string out(in);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

}  // namespace

std::string infer_image_mime_from_path(std::string_view path) {
    // Pull off the final extension, lowercase, compare against the short
    // whitelist covered by both OpenAI vision and Claude vision.
    auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return "image/png";
    std::string ext = to_lower_ascii(path.substr(dot + 1));
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg" || ext == "jpe") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "heic") return "image/heic";
    if (ext == "heif") return "image/heif";
    if (ext == "bmp") return "image/bmp";
    return "image/png";
}

AttachmentResult build_image_user_message(const std::string& text,
                                          const std::string& image_path) {
    AttachmentResult r;
    if (image_path.empty()) {
        r.error = AttachmentError::EmptyPath;
        return r;
    }

    std::error_code ec;
    std::filesystem::path p(image_path);
    if (!std::filesystem::exists(p, ec) || ec) {
        r.error = AttachmentError::NotFound;
        r.detail = "file not found";
        return r;
    }
    auto sz = std::filesystem::file_size(p, ec);
    if (ec) {
        r.error = AttachmentError::ReadFailed;
        r.detail = "could not stat file";
        return r;
    }
    if (sz > kMaxImageAttachmentBytes) {
        r.error = AttachmentError::TooLarge;
        r.detail = "image exceeds 20 MB";
        return r;
    }

    std::ifstream f(p, std::ios::binary);
    if (!f) {
        r.error = AttachmentError::ReadFailed;
        r.detail = "could not open file";
        return r;
    }
    std::vector<std::uint8_t> bytes;
    bytes.resize(static_cast<std::size_t>(sz));
    if (sz > 0) {
        f.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(sz));
        if (!f) {
            r.error = AttachmentError::ReadFailed;
            r.detail = "read failed";
            return r;
        }
    }

    std::string mime = infer_image_mime_from_path(image_path);
    std::string payload = base64_encode(bytes.data(), bytes.size());
    std::string data_url = "data:" + mime + ";base64," + payload;

    hermes::llm::Message m;
    m.role = hermes::llm::Role::User;

    // Text part first — matches the OpenAI vision example and the
    // convention used elsewhere in the Python reference.
    hermes::llm::ContentBlock text_block;
    text_block.type = "text";
    text_block.text = text;
    m.content_blocks.push_back(std::move(text_block));

    // Image part as OpenAI-style `image_url` — the Anthropic adapter in
    // cpp/llm/src/anthropic_adapter_depth.cpp translates this to the
    // corresponding Claude `image/source` block with media_type+base64.
    hermes::llm::ContentBlock image_block;
    image_block.type = "image_url";
    image_block.extra = nlohmann::json::object();
    image_block.extra["image_url"] = {{"url", data_url}};
    m.content_blocks.push_back(std::move(image_block));

    r.error = AttachmentError::Ok;
    r.message = std::move(m);
    return r;
}

}  // namespace hermes::cli
