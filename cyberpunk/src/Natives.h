#pragma once
#include <RED4ext/RED4ext.hpp>

// The script side: Magelight_* globals for redscript and CET (Game.Magelight_*), the delivery of page messages to
// redscript through Magelight_Fire (r6/scripts/Magelight/MagelightEvents.reds, Codeware), and the game-thread
// tick that delivers them (a RED4ext Running state).
namespace mlcp::natives {
void Install(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk);
}
