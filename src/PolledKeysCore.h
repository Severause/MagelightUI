#pragma once
// Polled keys (1.8.0): which of another plugin's GetAsyncKeyState / GetKeyState reads the page holding UI mode lets
// through the host's filter (the Polled keys block in Magelight.cpp hides keyboard keys from every other SKSE plugin
// by default). A page opts in per view (SetViewPolledKeys, manifest "polledKeys") or per request
// (kUIModeFlagPassPolledKeys). The rule names DLL file names, Windows virtual-key codes, or both.
//
// std only: tools/polledkeys-test runs all of it on the desktop.

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Magelight::PolledKeys {

    inline constexpr std::size_t kMaxModules = 32;
    inline constexpr std::size_t kMaxKeys = 64;

    // The side-neutral modifiers and their two sides: a rule listing VK_SHIFT, VK_CONTROL or VK_MENU also passes reads
    // of its left and right keys, since a plugin may ask either. A rule listing one side passes that side alone.
    inline constexpr std::uint16_t kVkShift = 0x10, kVkControl = 0x11, kVkMenu = 0x12;
    inline constexpr std::uint16_t kVkLShift = 0xA0, kVkRShift = 0xA1, kVkLControl = 0xA2, kVkRControl = 0xA3,
                                   kVkLMenu = 0xA4, kVkRMenu = 0xA5;

    // Empty modules = every plugin, empty keys = every key, so both empty passes every read.
    struct Rule {
        std::vector<std::wstring> modules;   // lower-case DLL file names, no path
        std::vector<std::uint16_t> keys;     // virtual-key codes, sorted, unique
    };

    inline std::wstring Lower(std::wstring_view s)
    {
        std::wstring out(s);
        std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return out;
    }

    // The lower-case file name of a module path (the text after the last \ or /).
    inline std::wstring FileNameLower(std::wstring_view path)
    {
        const auto slash = path.find_last_of(L"\\/");
        return Lower(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
    }

    inline bool KeyListed(const std::vector<std::uint16_t>& keys, int vk)
    {
        if (vk <= 0 || vk > 0xFE) return false;
        const auto has = [&](std::uint16_t k) { return std::binary_search(keys.begin(), keys.end(), k); };
        const auto v = static_cast<std::uint16_t>(vk);
        if (has(v)) return true;
        if (v == kVkLShift || v == kVkRShift) return has(kVkShift);
        if (v == kVkLControl || v == kVkRControl) return has(kVkControl);
        if (v == kVkLMenu || v == kVkRMenu) return has(kVkMenu);
        return false;
    }

    // moduleFileLower: the reading plugin's FileNameLower.
    inline bool Passes(const Rule& r, std::wstring_view moduleFileLower, int vk)
    {
        if (!r.modules.empty() &&
            std::find(r.modules.begin(), r.modules.end(), moduleFileLower) == r.modules.end())
            return false;
        return r.keys.empty() || KeyListed(r.keys, vk);
    }

    // A DLL file name as a rule names it: "" when fine, else why not. A path or a missing ".dll" is refused so a
    // typo cannot silently match nothing.
    inline std::string ModuleNameError(std::wstring_view name)
    {
        if (name.empty()) return "a module name is empty";
        if (name.find_first_of(L"\\/:") != std::wstring_view::npos) return "a module name is a path (name the file alone)";
        if (name.size() > 255) return "a module name is longer than 255 characters";
        const std::wstring lower = Lower(name);
        if (lower.size() < 5 || lower.compare(lower.size() - 4, 4, L".dll") != 0)
            return "a module name does not end in .dll";
        return {};
    }

    // Builds a rule from a caller's lists; "" on success, else why not (nothing is built then).
    inline std::string Build(const std::vector<std::wstring>& modules, const std::vector<int>& keys, Rule& out)
    {
        if (modules.size() > kMaxModules) return "more than " + std::to_string(kMaxModules) + " modules";
        if (keys.size() > kMaxKeys) return "more than " + std::to_string(kMaxKeys) + " keys";
        Rule r;
        for (const auto& m : modules) {
            if (std::string e = ModuleNameError(m); !e.empty()) return e;
            std::wstring lower = Lower(m);
            if (std::find(r.modules.begin(), r.modules.end(), lower) == r.modules.end()) r.modules.push_back(std::move(lower));
        }
        for (int k : keys) {
            if (k <= 0 || k > 0xFE) return "key " + std::to_string(k) + " is not a virtual-key code (1-254)";
            r.keys.push_back(static_cast<std::uint16_t>(k));
        }
        std::sort(r.keys.begin(), r.keys.end());
        r.keys.erase(std::unique(r.keys.begin(), r.keys.end()), r.keys.end());
        out = std::move(r);
        return {};
    }

    // A manifest key: a virtual-key code number, or a name (case-insensitive): A-Z, 0-9, F1-F24, Numpad0-Numpad9,
    // Shift/Ctrl/Alt and their LShift..RAlt sides, Space, Tab, Enter, Backspace, Insert, Delete, Home, End, PageUp,
    // PageDown, Up, Down, Left, Right, CapsLock, Grave, Minus, Equals. 0 = unknown.
    inline int ParseKeyName(std::string_view name)
    {
        std::string l(name);
        for (auto& c : l) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        if (l.size() == 1) {
            if (l[0] >= 'a' && l[0] <= 'z') return 'A' + (l[0] - 'a');
            if (l[0] >= '0' && l[0] <= '9') return l[0];
        }
        const auto numberAfter = [&](std::string_view prefix, int lo, int hi) -> int {
            if (l.size() <= prefix.size() || l.compare(0, prefix.size(), prefix) != 0) return -1;
            int n = 0;
            for (std::size_t i = prefix.size(); i < l.size(); ++i) {
                if (l[i] < '0' || l[i] > '9' || n > 100) return -1;
                n = n * 10 + (l[i] - '0');
            }
            return (n >= lo && n <= hi) ? n : -1;
        };
        if (const int n = numberAfter("numpad", 0, 9); n >= 0) return 0x60 + n;
        if (const int n = numberAfter("f", 1, 24); n >= 0) return 0x70 + n - 1;
        static constexpr std::pair<std::string_view, int> kNames[] = {
            { "shift", kVkShift }, { "ctrl", kVkControl }, { "control", kVkControl }, { "alt", kVkMenu },
            { "lshift", kVkLShift }, { "rshift", kVkRShift }, { "lctrl", kVkLControl }, { "rctrl", kVkRControl },
            { "lalt", kVkLMenu }, { "ralt", kVkRMenu },
            { "space", 0x20 }, { "tab", 0x09 }, { "enter", 0x0D }, { "backspace", 0x08 },
            { "insert", 0x2D }, { "delete", 0x2E }, { "home", 0x24 }, { "end", 0x23 }, { "pageup", 0x21 },
            { "pagedown", 0x22 }, { "up", 0x26 }, { "down", 0x28 }, { "left", 0x25 }, { "right", 0x27 },
            { "capslock", 0x14 }, { "grave", 0xC0 }, { "minus", 0xBD }, { "equals", 0xBB },
        };
        for (const auto& [n, vk] : kNames) if (l == n) return vk;
        return 0;
    }

}  // namespace Magelight::PolledKeys
