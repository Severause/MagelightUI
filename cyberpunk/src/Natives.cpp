#include "Natives.h"

#include "Api.h"
#include "Input.h"
#include "Log.h"
#include "Session.h"

#include <string>

namespace mlcp::natives {
namespace {
using RED4ext::CString;
using RED4ext::CStackFrame;
using RED4ext::IScriptable;

std::string Str(const CString& s) { return s.c_str() ? std::string(s.c_str()) : std::string(); }

template <class... T>
void Read(CStackFrame* f, T*... out) {
    (RED4ext::GetParameter(f, out), ...);
    f->code++;
}

void IsReadyFn(IScriptable*, CStackFrame* f, bool* out, int64_t) {
    f->code++;
    if (out) *out = api::IsReady();
}

void HasViewFn(IScriptable*, CStackFrame* f, bool* out, int64_t) {
    CString mod, view;
    Read(f, &mod, &view);
    if (out) *out = api::HasView(Str(mod), Str(view));
}

void ShowFn(IScriptable*, CStackFrame* f, bool* out, int64_t) {
    CString mod, view;
    bool show = false;
    Read(f, &mod, &view, &show);
    const bool ok = api::Show(Str(mod), Str(view), show);
    if (out) *out = ok;
}

void IsVisibleFn(IScriptable*, CStackFrame* f, bool* out, int64_t) {
    CString mod, view;
    Read(f, &mod, &view);
    if (out) *out = api::IsVisible(Str(mod), Str(view));
}

void SendFn(IScriptable*, CStackFrame* f, bool* out, int64_t) {
    CString mod, view, channel, text;
    Read(f, &mod, &view, &channel, &text);
    const bool ok = api::Send(Str(mod), Str(view), Str(channel), Str(text));
    if (out) *out = ok;
}

void OpenUIModeFn(IScriptable*, CStackFrame* f, bool* out, int64_t) {
    CString mod, view;
    Read(f, &mod, &view);
    const bool ok = api::OpenUIMode(Str(mod), Str(view));
    if (out) *out = ok;
}

void CloseUIModeFn(IScriptable*, CStackFrame* f, void*, int64_t) {
    f->code++;
    api::CloseUIMode();
}

void UIModeViewFn(IScriptable*, CStackFrame* f, CString* out, int64_t) {
    f->code++;
    if (out) *out = CString(api::UIModeView().c_str());
}

void ReloadFn(IScriptable*, CStackFrame* f, bool* out, int64_t) {
    CString mod, view;
    Read(f, &mod, &view);
    const bool ok = api::Reload(Str(mod), Str(view));
    if (out) *out = ok;
}

void NextMessageFn(IScriptable*, CStackFrame* f, CString* out, int64_t) {
    f->code++;
    const std::string next = api::NextForCet();
    if (out) *out = next.empty() ? CString() : CString(next.c_str());
}

struct Param {
    const char* type;
    const char* name;
};

void Register(RED4ext::CRTTISystem* rtti, const char* name, auto fn, std::initializer_list<Param> params,
              const char* ret) {
    auto* f = RED4ext::CGlobalFunction::Create(name, name, fn);
    f->flags = {.isNative = true, .isStatic = true};
    for (const auto& p : params) f->AddParam(p.type, p.name);
    if (ret) f->SetReturnType(ret);
    rtti->RegisterFunction(f);
}

void PostRegisterTypes() {
    auto* rtti = RED4ext::CRTTISystem::Get();
    const Param mod{"String", "mod"}, view{"String", "view"};
    Register(rtti, "Magelight_IsReady", &IsReadyFn, {}, "Bool");
    Register(rtti, "Magelight_HasView", &HasViewFn, {mod, view}, "Bool");
    Register(rtti, "Magelight_Show", &ShowFn, {mod, view, {"Bool", "show"}}, "Bool");
    Register(rtti, "Magelight_IsVisible", &IsVisibleFn, {mod, view}, "Bool");
    Register(rtti, "Magelight_Send", &SendFn, {mod, view, {"String", "channel"}, {"String", "text"}}, "Bool");
    Register(rtti, "Magelight_OpenUIMode", &OpenUIModeFn, {mod, view}, "Bool");
    Register(rtti, "Magelight_CloseUIMode", &CloseUIModeFn, {}, nullptr);
    Register(rtti, "Magelight_UIModeView", &UIModeViewFn, {}, "String");
    Register(rtti, "Magelight_Reload", &ReloadFn, {mod, view}, "Bool");
    Register(rtti, "Magelight_NextMessage", &NextMessageFn, {}, "String");
    Log("natives: 10 Magelight_* globals registered");
}

void RegisterTypes() {}

// Magelight_Fire, the reds global in MagelightEvents.reds: a reds global's RTTI name carries its signature, so it
// is found by its short name.
RED4ext::CBaseFunction* FindFire() {
    auto* rtti = RED4ext::CRTTISystem::Get();
    RED4ext::DynArray<RED4ext::CBaseFunction*> all;
    rtti->GetGlobalFunctions(all);
    for (auto* f : all) {
        const char* s = f ? f->shortName.ToString() : nullptr;
        if (s && std::string(s) == "Magelight_Fire") return f;
    }
    return nullptr;
}

// Game thread (the pump). false = not dispatched.
bool FireReds(const views::Message& m) {
    static RED4ext::CBaseFunction* fn = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        fn = FindFire();
        if (fn && fn->params.size() != 5) {
            Log("pump: Magelight_Fire takes %u arguments, not 5: an old MagelightEvents.reds",
                static_cast<unsigned>(fn->params.size()));
            fn = nullptr;
        }
        Log("pump: Magelight_Fire %s", fn ? "found, redscript listeners hear pages" : "missing");
    }
    if (!fn) return false;
    const std::string name = "Magelight/" + m.mod + "/" + m.channel;
    CString a[5] = {CString(name.c_str()), CString(m.mod.c_str()), CString(m.view.c_str()), CString(m.channel.c_str()),
                    CString(m.payload.c_str())};
    RED4ext::CStackType args[5];
    for (int i = 0; i < 5; ++i) {
        args[i].type = fn->params[i]->type;
        args[i].value = &a[i];
    }
    bool ok = false;
    RED4ext::CStackType result;
    result.type = fn->returnType ? fn->returnType->type : nullptr;
    result.value = &ok;
    RED4ext::CStack stack(nullptr, args, 5, result.type ? &result : nullptr);
    return fn->Execute(&stack) && ok;
}

unsigned g_loadsSaid = 0, g_endsSaid = 0;

bool OnEnter(RED4ext::CGameApplication*) { return true; }

bool OnUpdate(RED4ext::CGameApplication*) {
    const bool ready = session::Ready();
    if (ready && session::Loads() != g_loadsSaid) {   // never logs inside a load
        g_loadsSaid = session::Loads();
        Log("pump: a game is up (load %u over): page messages flow to scripts", g_loadsSaid);
    }
    if (ready && session::Ends() != g_endsSaid) {
        g_endsSaid = session::Ends();
        Log("pump: %u session end(s) seen, messages held through each", g_endsSaid);
    }
    input::Tick();
    api::Pump(ready);
    return false;
}

bool OnExit(RED4ext::CGameApplication*) { return true; }

RED4ext::v1::GameState g_state{&OnEnter, &OnUpdate, &OnExit};
}

void Install(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk) {
    auto* rtti = RED4ext::CRTTISystem::Get();
    rtti->AddRegisterCallback(&RegisterTypes);
    rtti->AddPostRegisterCallback(&PostRegisterTypes);
    api::SetRedsFire(&FireReds);
    const bool ok = sdk->gameStates && sdk->gameStates->Add(handle, RED4ext::EGameStateType::Running, &g_state);
    Log("load: game-thread tick %s", ok ? "registered" : "NOT registered: page messages never reach a script");
}
}
