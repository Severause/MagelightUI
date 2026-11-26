// MgA11yTest: the accessibility channel (1.10.0) in game. It registers an accessibility listener, as an assistive
// plugin would, and writes every message it hears to SKSE\MgA11yTest.log with the view, its mod and view name, and the
// view holding UI mode (GetUIModeView). Nothing is spoken. Never shipped to players.

#include <SKSE/SKSE.h>   // CommonLib before the Magelight header: it includes <windows.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "MagelightUI_Mod.h"

#include <filesystem>
#include <string>

SKSEPluginInfo(
    .Version = { 1, 0, 0, 0 },
    .Name = "MgA11yTest",
    .Author = "Magelight UI",
    .SupportEmail = "",
    .StructCompatibility = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary,
    .MinimumSKSEVersion = { 2, 0, 0, 0 }
)

namespace {
    namespace ML = MAGELIGHT_API;

    ML::MagelightMod g_mod;

    std::string Utf8(const std::filesystem::path& p)
    {
        const auto u = p.u8string();
        return std::string(reinterpret_cast<const char*>(u.data()), u.size());
    }

    // Render thread: no game state, nothing that blocks.
    void OnMessage(ML::ViewId view, const char* json, void*)
    {
        std::string who = "?";
        ML::ViewDesc d{};
        d.size = sizeof(d);
        if (g_mod.v4()->GetViewInfo(view, &d) == ML::Result::Ok && d.name) who = d.name;
        SKSE::log::info("[MgA11yTest] view {} ({}) ui={} {}", view, who, g_mod.uiModeView(), json ? json : "");
    }

    void OnDataLoaded()
    {
        if (!g_mod.acquire("MgA11yTest", "Accessibility test", 1, 10, 0, ML::CallbackThread::RenderThread)) {
            SKSE::log::error("[MgA11yTest] Magelight UI 1.10.0+ not present");
            return;
        }
        auto* api = g_mod.v4();
        SKSE::log::info("[MgA11yTest] QueryCapability a11y={} uiModeView={}", api->QueryCapability("a11y"),
                        api->QueryCapability("uiModeView"));
        SKSE::log::info("[MgA11yTest] listener {}",
                        g_mod.setAccessibilityListener(&OnMessage, nullptr) ? "registered" : "REFUSED");
    }

    void OnSkse(SKSE::MessagingInterface::Message* m)
    {
        if (m && m->type == SKSE::MessagingInterface::kDataLoaded) OnDataLoaded();
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    if (auto dir = SKSE::log::log_directory()) {
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(Utf8(*dir / L"MgA11yTest.log"), true);
        auto log = std::make_shared<spdlog::logger>("MgA11yTest", std::move(sink));
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
    }
    SKSE::GetMessagingInterface()->RegisterListener(OnSkse);
    return true;
}
