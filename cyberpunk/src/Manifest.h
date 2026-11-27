#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// <game>/r6/magelight/<ModId>/manifest.json, Magelight's schema (docs/MANIFEST.md). Keys this host does not do yet are
// logged and ignored; a bad view never stops the other views or mods.
namespace mlcp::manifest {

enum class Anchor { TopLeft, TopRight, BottomLeft, BottomRight };

struct ViewSpec {
    std::string mod, name;
    std::string path;            // relative to the mod folder, '/' separated
    int layer = 1;               // hud 0 < panel 1 < popup 2 < system 3
    Anchor anchor = Anchor::TopLeft;
    int x = 0, y = 0, w = 0, h = 0;   // logical pixels at 1080p
    bool fullscreen = false;
    bool clickThrough = false;
    bool startVisible = false;
    std::uint32_t hotkey = 0;    // DirectInput scancode (HotkeyNames.h), 0 = none
};

std::vector<ViewSpec> LoadAll(const std::filesystem::path& modsRoot);
}
