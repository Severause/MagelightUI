#include "Manifest.h"

#include "Log.h"

#include "../../src/HotkeyNames.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <set>

namespace mlcp::manifest {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;
using Magelight::HotkeyNames::Lower;

bool ValidSlug(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-')) return false;
    return true;
}

// Relative, inside the folder, no drive and no "..".
bool SafeRelative(const std::string& p) {
    if (p.empty() || p.find(':') != std::string::npos || p.front() == '/' || p.front() == '\\') return false;
    for (const auto& part : fs::path(p)) if (part == "..") return false;
    return true;
}

template <class T>
void Read(const json& o, const char* key, T& out, const std::string& where) {
    const auto it = o.find(key);
    if (it == o.end()) return;
    try {
        out = it->get<T>();
    } catch (const json::exception&) {
        Log("manifest %s: '%s' has the wrong type, default used", where.c_str(), key);
    }
}

const std::set<std::string> kViewKeys = {"path", "layer", "anchor", "x", "y", "w", "h", "fullscreen", "clickThrough",
                                         "startVisible", "hotkey"};
// Magelight keys this host does not do yet: named once, never an error.
const std::set<std::string> kLaterKeys = {"vr", "vrHotkey", "hotkeyPause", "prepaint", "mainMenu", "loadOnShow",
                                          "cursor", "hiddenTimers", "hibernateMs", "gamepad", "polledKeys",
                                          "shareFocus", "acceptFocus", "hitTest", "sounds"};

bool ParseView(const std::string& mod, const std::string& name, const json& v, ViewSpec& out) {
    const std::string where = mod + "/" + name;
    if (!v.is_object()) {
        Log("manifest %s: not an object, skipped", where.c_str());
        return false;
    }
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (kLaterKeys.count(it.key())) Log("manifest %s: '%s' is not supported on Cyberpunk yet, ignored", where.c_str(), it.key().c_str());
        else if (!kViewKeys.count(it.key())) Log("manifest %s: unknown key '%s', ignored", where.c_str(), it.key().c_str());
    }
    out.mod = mod;
    out.name = name;
    Read(v, "path", out.path, where);
    if (!SafeRelative(out.path)) {
        Log("manifest %s: 'path' must be a relative path inside the mod folder, view skipped", where.c_str());
        return false;
    }
    std::string layer = "panel", anchor = "top-left";
    Read(v, "layer", layer, where);
    Read(v, "anchor", anchor, where);
    layer = Lower(layer);
    anchor = Lower(anchor);
    out.layer = layer == "hud" ? 0 : layer == "popup" ? 2 : layer == "system" ? 3 : 1;
    out.anchor = (anchor == "top-right" || anchor == "topright")         ? Anchor::TopRight
               : (anchor == "bottom-left" || anchor == "bottomleft")     ? Anchor::BottomLeft
               : (anchor == "bottom-right" || anchor == "bottomright")   ? Anchor::BottomRight
                                                                         : Anchor::TopLeft;
    Read(v, "x", out.x, where);
    Read(v, "y", out.y, where);
    Read(v, "w", out.w, where);
    Read(v, "h", out.h, where);
    Read(v, "fullscreen", out.fullscreen, where);
    Read(v, "clickThrough", out.clickThrough, where);
    Read(v, "startVisible", out.startVisible, where);
    if (!out.fullscreen && (out.w <= 0 || out.h <= 0 || out.w > 7680 || out.h > 4320)) {
        Log("manifest %s: 'w' and 'h' are required (1..7680 x 1..4320) unless 'fullscreen', view skipped", where.c_str());
        return false;
    }
    if (const auto it = v.find("hotkey"); it != v.end()) {
        std::string err;
        out.hotkey = Magelight::HotkeyNames::Parse(*it, err);
        if (!err.empty()) Log("manifest %s: hotkey %s, no key bound", where.c_str(), err.c_str());
    }
    return true;
}
}

std::vector<ViewSpec> LoadAll(const fs::path& modsRoot) {
    std::vector<ViewSpec> out;
    std::error_code ec;
    if (!fs::is_directory(modsRoot, ec)) {
        Log("manifest: no mods folder at %ls", modsRoot.c_str());
        return out;
    }
    std::set<std::uint32_t> keys;
    // Nothing here may throw: this runs in the Present hook. Error codes, and the name as UTF-8 (string() throws on a
    // name the code page cannot hold).
    for (fs::directory_iterator entry(modsRoot, ec), end; !ec && entry != end; entry.increment(ec)) {
        const auto& dir = *entry;
        if (!dir.is_directory(ec)) continue;
        const auto file = dir.path() / "manifest.json";
        if (!fs::exists(file, ec)) continue;
        const auto name = dir.path().filename().u8string();
        const std::string folder(name.begin(), name.end());
        if (!ValidSlug(folder) || Lower(folder) == "resources") {
            Log("manifest: folder '%s' is not a valid mod id ([A-Za-z0-9_.-], 1-32, not 'resources'), skipped", folder.c_str());
            continue;
        }
        json m;
        try {
            std::ifstream in(file);
            m = json::parse(in, nullptr, true, true);
        } catch (const json::exception& e) {
            Log("manifest %s: not valid JSON (%s), skipped", folder.c_str(), e.what());
            continue;
        }
        std::string modId = folder;
        Read(m, "modId", modId, folder);
        if (Lower(modId) != Lower(folder)) {
            Log("manifest %s: modId '%s' differs from its folder name, skipped", folder.c_str(), modId.c_str());
            continue;
        }
        const auto views = m.find("views");
        if (views == m.end() || !views->is_object()) {
            Log("manifest %s: no 'views' object, skipped", folder.c_str());
            continue;
        }
        int count = 0;
        for (auto it = views->begin(); it != views->end(); ++it) {
            ViewSpec spec;
            // The manifest's own spelling: redscript's event names are case-sensitive ("Magelight/MyShop/buy").
            if (!ParseView(modId, it.key(), it.value(), spec)) continue;
            if (spec.hotkey && !keys.insert(spec.hotkey).second) {
                Log("manifest %s/%s: hotkey %u is already bound by another view, no key bound", folder.c_str(),
                    it.key().c_str(), spec.hotkey);
                spec.hotkey = 0;
            }
            out.push_back(std::move(spec));
            ++count;
        }
        Log("manifest %s: %d view(s)", folder.c_str(), count);
    }
    return out;
}
}
