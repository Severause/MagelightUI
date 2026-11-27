#include "Platform.h"

#include "Log.h"

#include "../../src/PathTextCore.h"

#include <Platform/fonts/FontLoaderWin.h>
#include <Ultralight/platform/FileSystem.h>
#include <Ultralight/platform/Logger.h>

#include <fstream>
#include <new>
#include <string>

namespace mlcp::platform {
namespace {
namespace fs = std::filesystem;

std::string Utf8(const ultralight::String& s) {
    const auto& u = s.utf8();
    return std::string(u.data(), u.length());
}

fs::path Under(const fs::path& root, const std::string& rel) {
    const auto p = (root / fs::path(std::u8string(rel.begin(), rel.end()))).lexically_normal();
    const auto inside = p.lexically_relative(root);
    // By the text only: under MO2 a file's real path is in its mod's own folder, never under the game's, so a
    // real-path check refuses every file, ICU's data included, and the game dies at boot.
    if (inside.empty() || *inside.begin() == "..") return {};
    return p;
}

class RootFileSystem final : public ultralight::FileSystem {
public:
    RootFileSystem(fs::path pluginDir, fs::path modsRoot)
        : plugin_(std::move(pluginDir)), mods_(std::move(modsRoot)) {}

    bool FileExists(const ultralight::String& path) override {
        std::error_code ec;
        const auto p = Map(path);
        return !p.empty() && fs::is_regular_file(p, ec);
    }

    ultralight::String GetFileMimeType(const ultralight::String& path) override {
        const auto u8 = Map(path).extension().u8string();   // string() throws on a name the code page cannot hold
        std::string ext(u8.begin(), u8.end());
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".html" || ext == ".htm") return "text/html";
        if (ext == ".js" || ext == ".mjs") return "text/javascript";
        if (ext == ".css") return "text/css";
        if (ext == ".json" || ext == ".map") return "application/json";
        if (ext == ".svg") return "image/svg+xml";
        if (ext == ".png") return "image/png";
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        if (ext == ".gif") return "image/gif";
        if (ext == ".webp") return "image/webp";
        if (ext == ".ico") return "image/x-icon";
        if (ext == ".woff") return "font/woff";
        if (ext == ".woff2") return "font/woff2";
        if (ext == ".ttf") return "font/ttf";
        if (ext == ".otf") return "font/otf";
        if (ext == ".txt") return "text/plain";
        if (ext == ".wasm") return "application/wasm";
        return "application/octet-stream";
    }

    ultralight::String GetFileCharset(const ultralight::String&) override { return "utf-8"; }

    ultralight::RefPtr<ultralight::Buffer> OpenFile(const ultralight::String& path) override {
        const auto p = Map(path);
        std::error_code ec;
        const auto size = p.empty() || !fs::is_regular_file(p, ec) ? std::uintmax_t(-1) : fs::file_size(p, ec);
        std::ifstream in;
        if (!ec && size != std::uintmax_t(-1)) in.open(p, std::ios::binary);
        if (!in) {
            Log("file: refused or missing: %s", Utf8(path).c_str());
            return nullptr;
        }
        auto* data = new (std::nothrow) char[size ? std::size_t(size) : 1];
        if (!data) {
            Log("file: no memory for %s (%llu bytes)", Utf8(path).c_str(), static_cast<unsigned long long>(size));
            return nullptr;
        }
        in.read(data, static_cast<std::streamsize>(size));
        return ultralight::Buffer::Create(data, std::size_t(size), nullptr,
                                          [](void*, void* d) { delete[] static_cast<char*>(d); });
    }

private:
    // Empty for a path that leaves its root.
    fs::path Map(const ultralight::String& path) const {
        // Magelight's reading of a request: a query dropped (Ultralight passes "font.woff2?v=4" through).
        const std::string s = Magelight::PathText::PathOfFileRequest(Utf8(path));
        if (s.rfind("resources/", 0) == 0) return Under(plugin_, s);
        return Under(mods_, s);
    }

    fs::path plugin_, mods_;
};

class FileLogger final : public ultralight::Logger {
public:
    void LogMessage(ultralight::LogLevel level, const ultralight::String& message) override {
        if (static_cast<int>(level) <= 2) Log("ultralight[%d]: %s", static_cast<int>(level), Utf8(message).c_str());
    }
};

class FileOnlyListener final : public ultralight::NetworkListener {
public:
    bool OnNetworkRequest(ultralight::View*, ultralight::NetworkRequest& request) override {
        const std::string url = Utf8(request.url());
        if (url.rfind("file:", 0) == 0 || url.rfind("data:", 0) == 0 || url.rfind("blob:", 0) == 0) return true;
        Log("network: refused %s (pages are file-only)", url.substr(0, 200).c_str());
        return false;
    }
};
}

ultralight::RefPtr<ultralight::Renderer> CreateRenderer(const fs::path& pluginDir, const fs::path& modsRoot) {
    ultralight::Config cfg;
    cfg.num_renderer_threads = 2;
    const auto cache = pluginDir / "cache";
    std::error_code ec;
    fs::create_directories(cache, ec);
    const auto u8 = cache.u8string();
    cfg.cache_path = ultralight::String(reinterpret_cast<const char*>(u8.c_str()));
    ultralight::Platform::instance().set_config(cfg);
    // Built here, never at namespace scope: the base constructors are delay-loaded imports. Never freed.
    ultralight::Platform::instance().set_font_loader(new ultralight::platform::FontLoaderWin());
    ultralight::Platform::instance().set_file_system(new RootFileSystem(pluginDir, modsRoot));
    ultralight::Platform::instance().set_logger(new FileLogger());
    return ultralight::Renderer::Create();
}

ultralight::NetworkListener* FileOnly() {
    static auto* listener = new FileOnlyListener();
    return listener;
}
}
