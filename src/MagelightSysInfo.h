#pragma once

#include <filesystem>
#include <string>

namespace SKSE { class LoadInterface; }

namespace Magelight::SysInfo {

    // Game runtime and flavour, SKSE, Windows (or the Wine under Proton), CPU and RAM, one line each. Plugin load,
    // main thread, after the crash telemetry is armed.
    void LogPlatform(const SKSE::LoadInterface* skse);

    // Every hardware graphics adapter with its VRAM and user-mode driver version, the one the game renders on
    // marked. kDataLoaded, main thread. DXGI is loaded from the system directory by full path, so a proxy
    // dxgi.dll (ReShade, a frame generator) is never asked, and no game device is touched.
    void LogGraphicsAdapters();

    // `p` as UTF-8 with the user's profile folder shown as %USERPROFILE%: log lines name the folder, not the
    // Windows user. UTF-8 because path::string() throws for a name the ANSI code page cannot spell.
    std::string ForLog(const std::filesystem::path& p);

    // UTF-8 text with every path under the user's profile folder shown as %USERPROFILE%: for text we do not
    // format ourselves (Ultralight's own log lines name its cache folder).
    std::string RedactProfile(std::string text);

}
