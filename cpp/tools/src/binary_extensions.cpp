#include "hermes/tools/binary_extensions.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace hermes::tools {

namespace {

const std::unordered_set<std::string>& storage() {
    static const std::unordered_set<std::string> exts = {
        // Images
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico",
        ".webp", ".tiff", ".tif", ".svg",
        // Videos
        ".mp4", ".mov", ".avi", ".mkv", ".webm", ".wmv",
        ".flv", ".m4v", ".mpeg", ".mpg",
        // Audio
        ".mp3", ".wav", ".ogg", ".flac", ".aac",
        ".m4a", ".wma", ".aiff", ".opus",
        // Archives
        ".zip", ".tar", ".gz", ".bz2", ".7z", ".rar",
        ".xz", ".z", ".tgz", ".iso",
        // Executables / shared libs / binaries
        ".exe", ".dll", ".so", ".dylib", ".bin", ".o",
        ".a", ".obj", ".lib", ".app", ".msi", ".deb", ".rpm",
        // Office documents
        ".pdf",
        ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx",
        ".odt", ".ods", ".odp",
        // Fonts
        ".ttf", ".otf", ".woff", ".woff2", ".eot",
        // Bytecode
        ".pyc", ".pyo", ".class", ".jar", ".war", ".ear",
        ".node", ".wasm", ".rlib",
        // Database files
        ".sqlite", ".sqlite3", ".db", ".mdb", ".idx",
        // Design / 3D
        ".psd", ".ai", ".eps", ".sketch", ".fig", ".xd",
        ".blend", ".3ds", ".max",
        // Flash
        ".swf", ".fla",
        // Generic blob
        ".lockb", ".dat", ".data",
    };
    return exts;
}

}  // namespace

bool is_binary_extension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    if (ext.empty()) return false;
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return storage().count(ext) > 0;
}

bool is_likely_binary_content(std::string_view first_chunk) {
    for (char c : first_chunk) {
        if (c == '\0') return true;
    }
    return false;
}

const std::unordered_set<std::string>& binary_extensions() {
    return storage();
}

}  // namespace hermes::tools
