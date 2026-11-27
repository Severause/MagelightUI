#pragma once
#include <cstdint>
#include <vector>

// The game's window messages, taken at InputSystemWin32Game::MessageProc (2.5 hash 1696011280). A view's hotkey
// shows or hides a HUD view, and opens or closes UI mode on any other: while a view holds UI mode, raw mouse and
// keys go to the pages and the game never sees them. Escape closes UI mode.
namespace mlcp::input {

struct Event {
    enum Kind { Move, Down, Up, Wheel, Key, Char } kind;
    int view = 0;                // the views:: id it goes to
    int x = 0, y = 0;            // logical page pixels, for Move/Down/Up/Wheel
    int button = 0;              // 1 left, 2 middle, 3 right (Ultralight's MouseEvent::Button)
    int delta = 0;               // wheel
    std::uintptr_t wparam = 0;   // Key: a WM_KEYDOWN/WM_KEYUP pair rebuilt from raw input
    std::intptr_t lparam = 0;
    bool up = false;
    unsigned modifiers = 0;      // Ultralight's KeyEvent::Modifiers
    // Key/Char: the modifier sides and locks held when it was queued (Magelight::Input::KeyBits). Ultralight's key
    // constructor reads them from its own thread's keyboard state, which the page thread never has.
    std::uint16_t keyBits = 0;
    char32_t ch = 0;             // Char
};

struct Cursor {
    bool shown = false;
    int x = 0, y = 0;            // backbuffer pixels
};

void SetScreen(int width, int height);Cursor CursorNow();
int UiModeView();                // 0 = none

// UI mode from a script, as the view's hotkey and Escape do it; both post '__uimode' to the scripts. Any thread.
bool OpenUIMode(int id);         // false for a HUD or click-through view, or no such view
void CloseUIMode();
// Atomics only, so the hooks that call them take no lock (a fault handler, the engine's state machine); Tick acts.
void Disable();                  // drawing is off for the session: UI mode closes and never opens again
void SetLoading(bool loading);   // while a load runs UI mode is closed and refused
void AskClose();                 // UI mode closes at the next Tick (a session ended)
void Tick();                     // game thread, every frame

void* Detour();
void** Original();

// Page thread: everything queued since the last call.
void Drain(std::vector<Event>& out);

// Raw input as the hook decodes it; tools/view_test drives these directly. True: the game must not see it.
bool OnKey(unsigned short vk, unsigned short makeCode, bool e0, bool up);
bool OnMouse(int dx, int dy, unsigned short buttonFlags, short wheel);
bool OnChar(char32_t ch);
void OnFocusLost();              // WM_KILLFOCUS / WM_ACTIVATEAPP off: nothing is held any more
}
