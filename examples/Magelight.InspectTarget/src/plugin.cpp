// Magelight.InspectTarget — a mod that uses BOTH Magelight tiers together.
//
// The split (and the whole point of the example):
//   • Papyrus owns the TRIGGER: a hotkey reads the crosshair actor, the
//     Magelight Papyrus tier finds the C++-created panel by modId
//     (Magelight.FindView), and the script hands the actor to this plugin's
//     own native `MagelightInspect.Open(Actor)` (MagelightInspect.psc).
//   • C++ owns the MOD, the VIEW, the live DATA, and opening and closing it:
//     it registers the mod + view, gathers the actor's live stats — the thing
//     Papyrus is bad at — into JSON, pushes it to the page and takes UI mode.
//
// A mod registered from C++ is plugin-owned: since host 0.30.0 a script may
// read it (FindView, IsVisible) but not show, focus or script its views, so
// the plugin exposes its own native for the script to call.
//
// Neither tier could do this alone: Papyrus can't cheaply read a rich RE::
// snapshot or register a JS listener; C++ wiring a quest hotkey is clumsy
// next to RegisterForKey. So each does what it's best at, over one modId.
//
// Build as an ordinary CommonLibSSE-NG SKSE plugin; copy api/MagelightUI_API.h
// and api/MagelightUI_Mod.h onto the include path.

#include <RE/Skyrim.h>               // CommonLib before the Magelight header: it includes <windows.h>
#include <SKSE/SKSE.h>
#include "MagelightUI_Mod.h"

#include <filesystem>
#include <string>

namespace {
    constexpr const char* kModId = "MagelightInspect";
    MAGELIGHT_API::MagelightMod g_mod;
    MAGELIGHT_API::ViewId       g_view = 0;

    std::string PagePath()
    {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        auto p = std::filesystem::path(buf).parent_path()
               / L"Data" / L"Magelight" / L"MagelightInspect" / L"views" / L"inspect" / L"index.html";
        return p.string();
    }

    std::string JsonEscape(const std::string& in)
    {
        std::string o; o.reserve(in.size() + 8);
        for (char c : in) {
            switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': break;
            case '\t': o += "\\t";  break;
            default:   o += c;      break;
            }
        }
        return o;
    }

    // ── The page's Close button: window.inspectClose() -> here. JS listeners
    //    run inline on the RENDER thread, not the callback thread chosen in
    //    acquire(): Magelight calls are fine here (any thread), but touching
    //    RE:: would need a PostGameTask first (host 0.30.1+; docs/CPP.md). The onClose
    //    slot hides the view. ──
    void OnCloseButton(MAGELIGHT_API::ViewId /*view*/, const char* /*arg*/, void* /*user*/)
    {
        g_mod.v4()->ReleaseUIMode(g_mod.id());
    }

    // ── The Papyrus->C++ bridge: MagelightInspect.Open(akActor). Gathers a
    //    live snapshot (C++'s job), pushes it to the page, shows the view and
    //    takes UI mode. Game thread (a Papyrus native call). False = not
    //    ready, or another mod holds UI mode (GetLastError names it). ──
    bool Open(RE::StaticFunctionTag*, RE::Actor* actor)
    {
        if (!g_view || !g_mod || !actor) return false;

        auto av = [&](RE::ActorValue v) {
            auto* o = actor->AsActorValueOwner();
            return o ? static_cast<int>(o->GetActorValue(v) + 0.5f) : 0;
        };
        const char* dn   = actor->GetDisplayFullName();
        auto*       race = actor->GetRace();
        auto*       weap = actor->GetEquippedObject(false);   // right hand

        std::string j = "{";
        j += "\"name\":\""  + JsonEscape(dn && *dn ? dn : "Unknown") + "\",";
        j += "\"race\":\""  + JsonEscape(race && race->GetName() ? race->GetName() : "") + "\",";
        j += "\"level\":"   + std::to_string(actor->GetLevel()) + ",";
        j += "\"health\":"  + std::to_string(av(RE::ActorValue::kHealth))  + ",";
        j += "\"magicka\":" + std::to_string(av(RE::ActorValue::kMagicka)) + ",";
        j += "\"stamina\":" + std::to_string(av(RE::ActorValue::kStamina)) + ",";
        j += "\"weapon\":\"" + JsonEscape(weap && weap->GetName() ? weap->GetName() : "Unarmed") + "\",";
        j += "\"inCombat\":" + std::string(actor->IsInCombat() ? "true" : "false");
        j += "}";

        // InvokeJS queues on the view until its DOM is ready, so filling
        // before the page has loaded is fine.
        g_mod.v4()->InvokeJS(g_view, ("window.setActor && window.setActor(" + j + ")").c_str());
        g_mod.v4()->ShowView(g_view, true);
        if (g_mod.v4()->RequestUIMode(g_view, MAGELIGHT_API::kUIModeFlagNone) != MAGELIGHT_API::Result::Ok) {
            g_mod.v4()->ShowView(g_view, false);
            return false;
        }
        return true;
    }

    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* vm)
    {
        vm->RegisterFunction("Open", "MagelightInspect", Open);
        return true;
    }

    void OnDataLoaded()
    {
        if (!g_mod.acquire(kModId, "Inspect Target", 0, 16, 0)) {
            SKSE::log::warn("MagelightInspect: Magelight UI 0.16.0+ not present — disabled");
            return;
        }
        const std::string page = PagePath();
        g_view = g_mod.createView("inspect", page.c_str(),
                                  MAGELIGHT_API::Layer::Panel, { .fullscreen = true });
        if (!g_view) {
            SKSE::log::error("MagelightInspect: view refused — {}", g_mod.v4()->GetLastErrorMessage(g_mod.id()));
            return;
        }
        // The page's Close button. A script cannot register listeners on a
        // plugin-owned view, so C++ holds it.
        g_mod.v4()->RegisterJSListenerEx(g_view, "inspectClose", &OnCloseButton, nullptr);
        // Every way out of UI mode (the Close button, Escape, the host's toggle
        // key, a lost race for UI mode) hides the panel. Runs on the callback
        // thread chosen in acquire() (GameThread).
        g_mod.onClose(g_view, [](MAGELIGHT_API::ViewId v) { g_mod.v4()->ShowView(v, false); });
        SKSE::log::info("MagelightInspect: mod '{}' + view 'inspect' ready — the script's hotkey opens it", kModId);
    }

    void OnMessage(SKSE::MessagingInterface::Message* m)
    {
        if (m && m->type == SKSE::MessagingInterface::kDataLoaded) OnDataLoaded();
    }
}

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SKSE::GetPapyrusInterface()->Register(RegisterPapyrus);   // exposes MagelightInspect.Open(Actor)
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
