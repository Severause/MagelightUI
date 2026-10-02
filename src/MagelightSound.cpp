// MagelightSound.cpp — see MagelightSound.h.
#include "MagelightSound.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Magelight::Sound
{
    namespace
    {
        struct Builtin { const char* name; RE::FormID id; };
        // Skyrim.esm SNDR records — byte-verified against the ESM 2026-09-12;
        // these seven are ALL the UIMenu* descriptors it carries.
        constexpr Builtin kBuiltins[] = {
            { "ok",       0x03C751 },   // UIMenuOKSD
            { "click",    0x03C751 },   //   (alias)
            { "cancel",   0x03C752 },   // UIMenuCancelSD
            { "prevnext", 0x03C753 },   // UIMenuPrevNextSD
            { "focus",    0x03C759 },   // UIMenuFocus
            { "hover",    0x03C759 },   //   (alias)
            { "open",     0x03F2BD },   // UIMenuBladeOpenSD
            { "close",    0x03F2BF },   // UIMenuBladeCloseSD
            { "inactive", 0x057F93 },   // UIMenuInactiveSD
        };
        constexpr const char* kBuiltinList = "ok click cancel prevnext focus hover open close inactive";
        // The whole vanilla UI*/ITM* family by EditorID (lowercased), so a page
        // can say magelight.sound('UIJournalOpen') or 'ITMGoldUpSD' and get the
        // exact sound the vanilla menu uses there. Generated — see the .inc.
        constexpr Builtin kVanilla[] = {
#include "MagelightSoundTable.inc"
        };

        std::mutex s_mutex;
        std::unordered_map<std::string, RE::FormID> s_cache;      // lowercased name -> FormID (0 = unknown, already warned)
        std::unordered_map<std::uint64_t, std::int64_t> s_hoverLast;  // view -> ms of the last hover play

        std::string Lower(std::string_view s)
        {
            std::string out(s);
            for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        // Game thread (TESDataHandler). `raw` keeps the caller's case for the
        // plugin name; `key` is the lowercased cache key.
        RE::FormID Resolve(const std::string& key, const std::string& raw)
        {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return 0;
            auto vanilla = [&](RE::FormID local) -> RE::FormID {
                auto* f = dh->LookupForm(local, "Skyrim.esm");
                return (f && f->Is(RE::FormType::SoundRecord)) ? f->GetFormID() : 0;
            };
            for (const auto& b : kBuiltins) if (key == b.name) return vanilla(b.id);
            for (const auto& b : kVanilla)  if (key == b.name) return vanilla(b.id);
            // "Plugin.esp|0xFormID" — the LOCAL form id, hex, 0x optional.
            const auto bar = raw.find('|');
            if (bar == std::string::npos || bar == 0 || bar + 1 >= raw.size()) return 0;
            const std::string plugin = raw.substr(0, bar);
            const std::string hex = raw.substr(bar + 1);
            char* end = nullptr;
            const unsigned long local = std::strtoul(hex.c_str(), &end, 16);
            if (!end || *end != '\0' || local == 0 || local > 0xFFFFFFu) return 0;
            auto* f = dh->LookupForm(static_cast<RE::FormID>(local), plugin);
            return (f && f->Is(RE::FormType::SoundRecord)) ? f->GetFormID() : 0;
        }
    }

    void Play(std::string_view nameIn, std::uint64_t view)
    {
        std::string raw(nameIn);
        while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.pop_back();
        std::size_t lead = 0;
        while (lead < raw.size() && std::isspace(static_cast<unsigned char>(raw[lead]))) ++lead;
        raw.erase(0, lead);
        if (raw.empty() || raw.size() > 128) return;

        const std::string key = Lower(raw);
        if (key == "none" || key == "silent") return;   // explicit opt-out (data-ml-sound="none"), never warns
        RE::FormID id = 0;
        bool known = false;
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            if (auto it = s_cache.find(key); it != s_cache.end()) { id = it->second; known = true; }
        }
        if (!known) {
            id = Resolve(key, raw);
            {
                std::lock_guard<std::mutex> lk(s_mutex);
                s_cache[key] = id;
            }
            if (!id) {
                SKSE::log::warn("Magelight[sound]: unknown sound '{}' (view {}) — built-ins: {}; any vanilla UI*/ITM* descriptor by EditorID; or Plugin.esp|0xFormID of a SNDR. Silent from here on.",
                                raw, view, kBuiltinList);
            } else {
                SKSE::log::info("Magelight[sound]: '{}' -> {:08X}", raw, id);
            }
        }
        if (!id) return;
        if (auto* am = RE::BSAudioManager::GetSingleton()) am->Play(id);
    }

    bool HoverAllowed(std::uint64_t view)
    {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(s_mutex);
        auto& last = s_hoverLast[view];
        if (now - last < 60) return false;
        last = now;
        return true;
    }
}
