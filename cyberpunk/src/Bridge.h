#pragma once
#include "Views.h"

#include <Ultralight/Ultralight.h>

#include <string>
#include <vector>

// Magelight's page contract (docs/SDK.md, "The page contract"): window.__MAGELIGHT__ and window.magelight
// (send / on / off / pending / _dispatch), installed before the page's own scripts. Page thread only, except
// TakeMessages.
namespace mlcp::bridge {

void InstallCore(ultralight::View* view, int id, const std::string& mod, const std::string& name);

// window[channel](text) when the page defined one, else magelight._dispatch, which buffers until a subscriber.
void Dispatch(ultralight::View* view, const std::string& channel, const std::string& text);
// Page thread, before an input that can open WebKit's context menu: arms the guard in frames no load reached.
void ArmMenuGuard(ultralight::View* view);

// The host's own messages ('__ready', '__uimode'), into the same outbox as the pages'. Any thread.
void Post(const std::string& mod, const std::string& view, const std::string& channel, const std::string& payload);

// One relaxed load: whether anything waits. Safe during a load (no lock, no allocation).
bool Waiting();
void TakeMessages(std::vector<views::Message>& out);
}
