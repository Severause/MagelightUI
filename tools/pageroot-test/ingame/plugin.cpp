// MgPageRootTest: AddPageRoot (1.10.0) in game. Pages kept outside Data\Magelight, laid out the way a page set made
// for another UI host is (sibling page folders sharing ../Shared), are shown as three HUD boxes at the top right:
//   Data\MgPageRootTest\views\Hud\index.html             loads ../Shared/*     (under the root: must load)
//   Data\MgPageRootTest\views\Minigames\Lock\index.html  loads ../../Shared/*  (under the root: must load)
//   Data\MgPageRootTest\control\Page\index.html          loads ../Shared/*     (no root: must stay refused)
// Each box says PASS or FAIL; every check is also a line in SKSE\MgPageRootTest.log. Never shipped to players.

#include <SKSE/SKSE.h>   // CommonLib before the Magelight header: it includes <windows.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "MagelightUI_Mod.h"

#include <filesystem>
#include <string>

SKSEPluginInfo(
    .Version = { 1, 0, 0, 0 },
    .Name = "MgPageRootTest",
    .Author = "Magelight UI",
    .SupportEmail = "",
    .StructCompatibility = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary,
    .MinimumSKSEVersion = { 2, 0, 0, 0 }
)

namespace {
    namespace ML = MAGELIGHT_API;

    ML::MagelightMod g_mod;
    int g_fails = 0;

    std::filesystem::path DataDir()
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return std::filesystem::path(buf).parent_path() / L"Data";
    }

    std::string Utf8(const std::filesystem::path& p)
    {
        const auto u = p.u8string();
        return std::string(reinterpret_cast<const char*>(u.data()), u.size());
    }

    void Check(bool ok, const std::string& what)
    {
        if (!ok) ++g_fails;
        SKSE::log::info("[MgPageRootTest] {}  {}", ok ? "PASS" : "FAIL", what);
    }

    void ExpectRefused(const std::string& folder, const char* what)
    {
        const ML::Result r = g_mod.v4()->AddPageRoot(g_mod.id(), folder.c_str());
        Check(r == ML::Result::InvalidArgument,
              std::string("AddPageRoot refuses ") + what + " (" + g_mod.v4()->GetLastErrorMessage(g_mod.id()) + ")");
    }

    // A page's report: {"page":"hud","pass":true,"js":true,"css":true,"net":"loopback"}.
    void OnResult(ML::ViewId, const char* arg, void*)
    {
        const std::string s = arg ? arg : "";
        Check(s.find("\"pass\":true") != std::string::npos, "page report " + s);
    }

    ML::ViewId Box(const char* name, const std::filesystem::path& page, int y)
    {
        ML::MagelightMod::ViewOpts o;
        o.anchor = ML::Anchor::TopRight;
        o.x = 20;
        o.y = y;
        o.w = 460;
        o.h = 64;
        o.clickThrough = true;
        o.startVisible = true;
        const ML::ViewId v = g_mod.createView(name, Utf8(page).c_str(), ML::Layer::Hud, o);
        Check(v != 0, std::string("view '") + name + "' created");
        if (v) g_mod.v4()->RegisterJSListenerEx(v, "result", &OnResult, nullptr);
        return v;
    }

    void OnDataLoaded()
    {
        if (!g_mod.acquire("MgPageRootTest", "Page root test", 1, 10, 0)) {
            SKSE::log::error("[MgPageRootTest] Magelight UI 1.10.0+ not present");
            return;
        }
        auto* api = g_mod.v4();
        Check(api->QueryCapability("pageRoot") == 1, "QueryCapability(pageRoot) is 1");

        const auto data = DataDir();
        ExpectRefused(Utf8(data), "the Data folder itself");
        ExpectRefused(Utf8(data.parent_path()), "the game folder");
        ExpectRefused(Utf8(data.root_path()), "a drive root");
        ExpectRefused(Utf8(data / L"Magelight" / L"MgPageRootTest"), "a folder in Data\\Magelight");
        ExpectRefused(Utf8(data / L"SKSE"), "a folder holding Magelight's own");
        ExpectRefused(Utf8(data / L"SKSE" / L"Plugins" / L"Magelight" / L"views"), "a folder inside Magelight's own");
        ExpectRefused(Utf8(data / L"MgPageRootTest" / L"missing"), "a folder that does not exist");
        ExpectRefused("MgPageRootTest\\views", "a relative folder");

        const auto views = data / L"MgPageRootTest" / L"views";
        Check(g_mod.addPageRoot(Utf8(views).c_str()), "addPageRoot takes Data\\MgPageRootTest\\views");
        Check(api->AddPageRoot(g_mod.id(), (Utf8(views) + "\\").c_str()) == ML::Result::Ok,
              "adding it again (trailing separator) is Ok");

        Box("hud", views / L"Hud" / L"index.html", 20);
        const ML::ViewId lock = Box("lock", views / L"Minigames" / L"Lock" / L"index.html", 92);
        Box("control", data / L"MgPageRootTest" / L"control" / L"Page" / L"index.html", 164);

        // A relative cursor image resolves against the view's root: Shared\ sits beside Minigames\, not under Lock\.
        if (lock) {
            ML::CursorDesc d{ sizeof(d) };
            d.state = ML::kCursorArrow;
            d.imagePath = "Shared/cursor.png";
            Check(api->SetViewCursor(lock, &d) == ML::Result::Ok,
                  "a relative cursor image resolves against the root (Shared/cursor.png from Minigames\\Lock)");
        }

        // After the root and the views: the pages, which load after this, must be served the loopback CSP.
        Check(api->SetNetworkPolicy(g_mod.id(), ML::NetworkPolicy::LoopbackOnly) == ML::Result::Ok,
              "SetNetworkPolicy(LoopbackOnly)");
        SKSE::log::info("[MgPageRootTest] {} failed so far; three page reports follow once a save loads", g_fails);
    }

    void OnMessage(SKSE::MessagingInterface::Message* m)
    {
        if (m && m->type == SKSE::MessagingInterface::kDataLoaded) OnDataLoaded();
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    if (auto dir = SKSE::log::log_directory()) {
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(Utf8(*dir / L"MgPageRootTest.log"), true);
        auto log = std::make_shared<spdlog::logger>("MgPageRootTest", std::move(sink));
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
    }
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
