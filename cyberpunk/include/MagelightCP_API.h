#pragma once
// MagelightCP's C++ API for other RED4ext plugins. Get it once the game has loaded the plugins:
//   auto get = reinterpret_cast<MagelightCP::GetApiFn>(GetProcAddress(GetModuleHandleW(L"MagelightCP.dll"),
//                                                                      "MagelightCP_GetApi"));
//   const MagelightCP::Api1* api = get ? get(1) : nullptr;
// A view is named by its mod id (the folder under r6/magelight) and its name in the manifest. Every call returns at
// once: it queues work for the page thread or reads what that thread published. Listeners run on the game thread,
// never while a game is loading.
#include <cstdint>

namespace MagelightCP {

// What a page sent with magelight.send, or the host's '__ready' / '__uimode'. The strings live for the call only.
using ListenerFn = void (*)(const char* mod, const char* view, const char* channel, const char* payload, void* user);

struct Api1 {
    std::uint32_t version;   // 1
    bool (*IsReady)();
    bool (*HasView)(const char* mod, const char* view);
    bool (*Show)(const char* mod, const char* view, bool show);
    bool (*IsVisible)(const char* mod, const char* view);
    bool (*Send)(const char* mod, const char* view, const char* channel, const char* text);
    bool (*OpenUIMode)(const char* mod, const char* view);
    void (*CloseUIMode)();
    bool (*Reload)(const char* mod, const char* view);
    // mod "" hears every mod. Not removable: register once, at load.
    void (*AddListener)(const char* mod, ListenerFn fn, void* user);
};

using GetApiFn = const Api1* (*)(std::uint32_t version);
}
