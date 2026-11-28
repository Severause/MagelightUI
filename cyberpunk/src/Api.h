#pragma once
#include "../include/MagelightCP_API.h"

#include "Views.h"

#include <string>

// What a script or a plugin can do, once, for the RTTI natives (Natives.cpp) and the C++ export alike. No engine here,
// so tools/view_test runs all of it.
namespace mlcp::api {

bool IsReady();
bool HasView(const std::string& mod, const std::string& view);
bool Show(const std::string& mod, const std::string& view, bool show);   // hiding the UI-mode view closes UI mode
bool IsVisible(const std::string& mod, const std::string& view);
bool Send(const std::string& mod, const std::string& view, const std::string& channel, const std::string& text);
bool OpenUIMode(const std::string& mod, const std::string& view);
void CloseUIMode();
std::string UIModeView();   // "mod/view", "" = none
bool Reload(const std::string& mod, const std::string& view);

// Messages out, on the game thread.
using RedsFire = bool (*)(const views::Message& m);   // false = could not dispatch (no Codeware, no reds)
void SetRedsFire(RedsFire fire);
void AddListener(const std::string& mod, MagelightCP::ListenerFn fn, void* user);

// Delivers what waits, when the game is not loading. Game thread, every frame: with nothing waiting it is one
// relaxed load, no lock and no allocation (the rule from loads that crashed on an allocating tick).
void Pump(bool gameReady);

// The CET door's queue: one message as "mod\x1fview\x1fchannel\x1fpayload", "" when none. The first call turns
// the queue on; until then nothing is kept for CET.
std::string NextForCet();

const MagelightCP::Api1* Exported();
}
