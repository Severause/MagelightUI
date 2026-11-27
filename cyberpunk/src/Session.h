#pragma once
#include <RED4ext/RED4ext.hpp>

// Whether a game is loading, from the engine's own game-state machine: gsm::State_SessionLoading starts every load
// and gsm::State_SessionActive ends every one; gsm::State_Session's shutdown is a world coming down (measured on 2.5,
// docs/CYBERPUNK.md "Game states"). From an end or a load's start, nothing is handed to scripts until a load is over.
namespace mlcp::session {

void Install(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk);

// True after a load finished and before the next one starts. False before the first game: the main menu holds
// messages until a game is up.
bool Ready();

// For the log, from a caller that is not in a load: how many loads were seen.
unsigned Loads();
unsigned Ends();
}
