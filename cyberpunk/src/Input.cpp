#include "Input.h"

#include "Bridge.h"
#include "Log.h"
#include "Views.h"

#include "../../src/InputCore.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <bitset>
#include <utility>
#include <mutex>

namespace mlcp::input {
namespace {
// A sink's vtable[0], passed through raw: whether `this` comes first is decided at the first call (Layout).
using SinkFn = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
SinkFn g_original = nullptr;
enum class Layout { Unknown, Member, Free } g_layout = Layout::Unknown;

std::mutex g_mutex;
auto& g_queue = *new std::vector<Event>();

std::recursive_mutex g_uiMutex;   // UI mode changes: the window's thread (keys) and the game thread (scripts)
std::atomic<int> g_ui{0};   // the view holding UI mode
std::atomic<bool> g_disabled{false};   // drawing died: no UI mode, hotkeys are the game's
std::atomic<bool> g_blocked{false};    // a load is running: no UI mode
std::atomic<bool> g_closeAsked{false};  // by a hook that may take no lock: Tick closes UI mode
std::atomic<int> g_cx{960}, g_cy{540};
std::atomic<int> g_sw{1920}, g_sh{1080};
bool g_cursorPlaced = false;
std::atomic<int> g_capture{0};   // the view a held button went down on: it gets the up
std::atomic<int> g_hover{0};     // the view the pointer was last over: it gets a leave when that ends
std::bitset<256> g_down;    // DirectInput scancodes held, so a held hotkey toggles once
// An up goes where its down went, so neither the game nor a page is left with a key or button held.
std::bitset<256> g_taken;         // raw keys whose down we kept from the game
std::bitset<256> g_pageDown;      // raw keys whose down reached a page
auto& g_pageKey = *new std::array<Event, 256>();   // that down, by raw key: its up goes to the same view
unsigned short g_pageButtons = 0;   // RI_MOUSE_*_DOWN bits whose down went to a page
std::bitset<256> g_legacyTaken;   // WM_KEYDOWN by virtual key
unsigned short g_gameButtons = 0;   // RI_MOUSE_*_DOWN bits whose down went to the game
std::int64_t g_fx = 0, g_fy = 0;    // the part of a move below one pixel, in 1080ths, kept for the next
HWND g_hwnd = nullptr;
bool g_shift = false, g_ctrl = false, g_alt = false, g_sawChar = false;
char32_t g_high = 0, g_lastTyped = 0;
wchar_t g_deadKey = 0;   // a dead key's spacing form, waiting for the letter it marks

constexpr unsigned kModAlt = 1, kModCtrl = 2, kModShift = 8;
constexpr unsigned kDikEscape = 0x01;

void Push(const Event& e) {
    // Past the soft cap (a page thread stuck in a script) only releases still go in: a dropped one leaves it held.
    const bool release = e.kind == Event::Up || (e.kind == Event::Key && e.up);
    std::lock_guard lock(g_mutex);
    if (g_queue.size() < 8192 || (release && g_queue.size() < 16384)) {
        g_queue.push_back(e);
        return;
    }
    static bool told = false;
    if (!told) Log("input: page thread behind, input dropped");
    told = true;
}

unsigned Mods() { return (g_alt ? kModAlt : 0) | (g_ctrl ? kModCtrl : 0) | (g_shift ? kModShift : 0); }

// The state a key is queued with, by DirectInput code: each modifier's side, then the locks.
std::uint16_t KeyBitsNow() {
    using namespace Magelight::Input;
    static constexpr struct { unsigned dik; std::uint16_t bit; } kSides[] = {
        {0x2A, kKbLShift}, {0x36, kKbRShift}, {0x1D, kKbLCtrl}, {0x9D, kKbRCtrl},
        {0x38, kKbLAlt},   {0xB8, kKbRAlt},   {0xDB, kKbLWin},  {0xDC, kKbRWin}};
    std::uint16_t bits = 0;
    for (const auto& s : kSides)
        if (g_down[s.dik]) bits |= s.bit;
    if (GetKeyState(VK_CAPITAL) & 1) bits |= kKbCaps;
    if (GetKeyState(VK_NUMLOCK) & 1) bits |= kKbNumLock;
    return bits;
}

bool TakesUIMode(const views::Row& r) { return !r.clickThrough && r.layer != 0; }   // a HUD view never does

const views::Row* ById(const std::vector<views::Row>& rows, int id);
Event At(Event::Kind kind, const views::Row& r);

// A move outside the page, which clears its :hover and fires its mouseleave.
void Leave(int view) {
    Event e{Event::Move};
    e.view = view;
    e.x = e.y = -1;
    Push(e);
}

// The pages get the release of every key and button they hold, and the hovered page its leave.
void ReleasePageHeld() {
    if (const int over = g_hover.exchange(0)) Leave(over);
    for (unsigned dik = 0; dik < 256; ++dik) {
        if (!g_pageDown[dik]) continue;
        Event e = g_pageKey[dik];
        e.up = true;
        e.lparam |= (std::intptr_t(1) << 30) | (std::intptr_t(1) << 31);
        e.keyBits = 0;
        e.modifiers = 0;
        Push(e);
    }
    g_pageDown.reset();
    const auto rows = views::Layout();
    if (const views::Row* held = ById(rows, g_capture.exchange(0))) {
        for (const auto& b : {std::pair{RI_MOUSE_LEFT_BUTTON_DOWN, 1}, std::pair{RI_MOUSE_MIDDLE_BUTTON_DOWN, 2},
                              std::pair{RI_MOUSE_RIGHT_BUTTON_DOWN, 3}}) {
            if (!(g_pageButtons & b.first)) continue;
            Event e = At(Event::Up, *held);
            e.button = b.second;
            Push(e);
        }
    }
    g_deadKey = 0;
}

// Windows sends no up for what was held when the game lost focus: nothing is held any more, on either side.
void LoseFocus() {
    std::lock_guard lock(g_uiMutex);
    ReleasePageHeld();
    g_down.reset();
    g_taken.reset();
    g_legacyTaken.reset();
    g_gameButtons = g_pageButtons = 0;
    g_shift = g_ctrl = g_alt = false;
}

void Close() {
    std::lock_guard lock(g_uiMutex);
    const int id = g_ui.exchange(0);
    if (!id) return;
    // The real ups are then kept from the game (g_taken, g_pageButtons).
    ReleasePageHeld();
    views::Focus(id, false);
    views::Show(id, false);
    for (const auto& r : views::Layout())
        if (r.id == id) bridge::Post(r.mod, r.name, "__uimode", "0");
    Log("input: UI mode closed (view %d)", id);
}

void Open(const views::Row& r) {
    std::lock_guard lock(g_uiMutex);
    if (g_disabled || g_blocked) return;
    Close();
    if (!g_cursorPlaced) {
        g_cx = r.x + r.w / 2;
        g_cy = r.y + r.h / 2;
        g_cursorPlaced = true;
    }
    views::Show(r.id, true);
    views::Focus(r.id, true);
    g_ui = r.id;
    bridge::Post(r.mod, r.name, "__uimode", "1");
    Log("input: UI mode on %s/%s (view %d)", r.mod.c_str(), r.name.c_str(), r.id);
}

void Toggle(const views::Row& r) {
    std::lock_guard lock(g_uiMutex);
    if (!TakesUIMode(r)) {
        views::Show(r.id, !r.visible);
        Log("input: %s/%s %s", r.mod.c_str(), r.name.c_str(), r.visible ? "hidden" : "shown");
        return;
    }
    if (g_ui == r.id) Close();
    else Open(r);
}

// The topmost shown view under the cursor that takes the pointer; the layout is in drawing order.
const views::Row* Under(const std::vector<views::Row>& rows, int x, int y) {
    for (auto it = rows.rbegin(); it != rows.rend(); ++it)
        if (it->visible && !it->clickThrough && x >= it->x && y >= it->y && x < it->x + it->w && y < it->y + it->h)
            return &*it;
    return nullptr;
}

const views::Row* ById(const std::vector<views::Row>& rows, int id) {
    for (const auto& r : rows)
        if (r.id == id) return &r;
    return nullptr;
}

Event At(Event::Kind kind, const views::Row& r) {
    Event e{kind};
    e.view = r.id;
    e.x = int((g_cx - r.x) / r.scale);
    e.y = int((g_cy - r.y) / r.scale);
    return e;
}

// The text a key types with these modifiers; 0 for none. Ctrl+Alt is AltGr; Ctrl or Alt alone types nothing.
// A dead key gives its spacing form and sets *dead.
wchar_t TextOf(unsigned short vk, unsigned short makeCode, bool shift, bool ctrl, bool alt, bool* dead = nullptr) {
    if (ctrl != alt) return 0;
    BYTE state[256] = {};
    if (shift) state[VK_SHIFT] = 0x80;
    if (ctrl) state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
    if (alt) state[VK_MENU] = state[VK_RMENU] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1) state[VK_CAPITAL] = 1;
    wchar_t text[4];
    // 4: no state change, so the game's thread keeps no dead key of ours; Text composes it instead.
    const int n = ToUnicode(vk, makeCode, state, text, 4, 4);
    if (n < 0) {
        if (dead) *dead = true;
        return text[0];
    }
    return n == 1 && text[0] >= 0x20 ? text[0] : 0;
}

// A dead key's spacing form and the letter after it as one character ("´" + "a" = "á"); 0 when they do not compose.
wchar_t Compose(wchar_t dead, wchar_t base) {
    static constexpr std::pair<wchar_t, wchar_t> kMarks[] = {
        {L'`', 0x300}, {0xB4, 0x301}, {L'\'', 0x301}, {L'^', 0x302}, {L'~', 0x303}, {0xAF, 0x304}, {0x2D8, 0x306},
        {0x2D9, 0x307}, {0xA8, 0x308}, {L'"', 0x308}, {0xB0, 0x30A}, {0x2DA, 0x30A}, {0x2DD, 0x30B}, {0x2C7, 0x30C},
        {0xB8, 0x327}, {0x2DB, 0x328}};
    if (base == L' ') return dead;
    for (const auto& [spacing, mark] : kMarks) {
        if (spacing != dead) continue;
        const wchar_t src[2] = {base, mark};
        wchar_t out[4];
        return NormalizeString(NormalizationC, src, 2, out, 4) == 1 ? out[0] : 0;
    }
    return 0;
}

void Typed(wchar_t ch) {
    Event c{Event::Char};
    c.view = g_ui;
    c.ch = ch;
    c.modifiers = Mods();
    c.keyBits = KeyBitsNow();
    Push(c);
    g_lastTyped = ch;
}

void Text(unsigned short vk, unsigned short makeCode) {
    // No WM_CHAR reaches the window (raw input with no legacy messages): the text comes from the key itself.
    bool dead = false;
    const wchar_t ch = TextOf(vk, makeCode, g_shift, g_ctrl, g_alt, &dead);
    if (!ch) return;
    const wchar_t pending = std::exchange(g_deadKey, wchar_t(0));
    if (dead) {
        if (pending) Typed(pending), Typed(ch);   // two dead keys type both, as Windows does
        else g_deadKey = ch;
        return;
    }
    if (!pending) return Typed(ch);
    if (const wchar_t c = Compose(pending, ch)) return Typed(c);
    Typed(pending);
    Typed(ch);
}

// True: the message belongs to the game — a button's up whose down it had. Also notes the downs it gets.
bool GameButton(unsigned short flags) {
    static constexpr unsigned short kDowns[] = {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN,
                                                RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_BUTTON_4_DOWN,
                                                RI_MOUSE_BUTTON_5_DOWN};
    bool game = false, pageUp = false;
    for (const unsigned short down : kDowns) {
        const unsigned short up = down << 1;
        if ((flags & up) && (g_gameButtons & down)) game = true, g_gameButtons &= ~down;
        else if ((flags & up) && (g_pageButtons & down)) pageUp = true, g_pageButtons &= ~down;
        if (flags & down) (g_ui ? g_pageButtons : g_gameButtons) |= down;
    }
    // After UI mode closed, the up of a press a page had is the page's (Close released it there), not the game's.
    return game || (!g_ui && !pageUp);
}

bool Pointer(bool moved, unsigned short buttonFlags, short wheel);

bool TakeRawInput(LPARAM lParam) {
    RAWINPUT raw;
    UINT size = sizeof raw;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1))
        return false;
    if (raw.header.dwType == RIM_TYPEKEYBOARD) {
        const auto& k = raw.data.keyboard;
        if (k.VKey == 0xFF) return false;   // the fake shift some keys send around E0 codes
        // Pause comes E1-prefixed with LCtrl's code; DirectInput calls it 0xC5, as NumLock is 0x45.
        if (k.VKey == VK_PAUSE) return OnKey(k.VKey, 0x45, true, (k.Flags & RI_KEY_BREAK) != 0);
        return OnKey(k.VKey, k.MakeCode, (k.Flags & RI_KEY_E0) != 0, (k.Flags & RI_KEY_BREAK) != 0);
    }
    if (raw.header.dwType == RIM_TYPEMOUSE) {
        const auto& m = raw.data.mouse;
        if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) return OnMouse(m.lLastX, m.lLastY, m.usButtonFlags, SHORT(m.usButtonData));
        // A tablet, a VM or remote desktop: where the system cursor is, in backbuffer pixels.
        if (GameButton(m.usButtonFlags)) return false;
        if (!g_ui) return true;   // the up of a page's press, after UI mode closed
        POINT p;
        RECT rc;
        if (g_hwnd && GetCursorPos(&p) && ScreenToClient(g_hwnd, &p) && GetClientRect(g_hwnd, &rc) && rc.right > 0 &&
            rc.bottom > 0) {
            g_cx = std::clamp(int(p.x * double(g_sw) / rc.right), 0, g_sw - 1);
            g_cy = std::clamp(int(p.y * double(g_sh) / rc.bottom), 0, g_sh - 1);
        }
        return Pointer(true, m.usButtonFlags, SHORT(m.usButtonData));
    }
    return false;
}

// True: the message is ours and the game must not see it.
bool Take(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INPUT) return TakeRawInput(lParam);
    if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATEAPP && !wParam)) {
        LoseFocus();
        return false;
    }
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam < 256) {   // an up goes where its down went
        const bool ours = g_legacyTaken[wParam];
        g_legacyTaken[wParam] = false;
        return ours;
    }
    if (!g_ui) return false;
    switch (msg) {
    case WM_CHAR:
        return OnChar(static_cast<char32_t>(wParam));
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wParam == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) return false;   // Alt+F4 still closes the game
        if (wParam < 256 && !(lParam & (1 << 30))) g_legacyTaken[wParam] = true;   // a repeat keeps the first down's owner
        return wParam >= 256 || g_legacyTaken[wParam];
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
        return true;
    default:
        return false;
    }
}

std::uintptr_t Hooked(std::uintptr_t a0, std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4) {
    if (g_layout == Layout::Unknown) {
        g_layout = IsWindow(reinterpret_cast<HWND>(a1)) ? Layout::Member : Layout::Free;
        Log("input: MessageProc takes %s", g_layout == Layout::Member ? "(this, hwnd, msg, wParam, lParam)"
                                                                       : "(hwnd, msg, wParam, lParam)");
    }
    const bool member = g_layout == Layout::Member;
    g_hwnd = reinterpret_cast<HWND>(member ? a1 : a0);
    bool taken;
    {
        std::lock_guard lock(g_uiMutex);   // a script's or a load's Close reads the held-key state this changes
        taken = Take(static_cast<UINT>(member ? a2 : a1), member ? a3 : a2, static_cast<LPARAM>(member ? a4 : a3));
    }
    if (taken) return 1;
    return g_original(a0, a1, a2, a3, a4);
}
}

bool OnKey(unsigned short vk, unsigned short makeCode, bool e0, bool up) {
    const unsigned dik = (makeCode & 0x7f) | (e0 ? 0x80u : 0u);
    const bool repeat = !up && g_down[dik];
    g_down[dik] = !up;
    if (vk == VK_SHIFT) g_shift = !up;
    if (vk == VK_CONTROL) g_ctrl = !up;
    if (vk == VK_MENU) g_alt = !up;

    const auto keyEvent = [&] {
        Event e{Event::Key};
        e.view = g_ui;
        e.up = up;
        e.wparam = vk;
        e.lparam = 1 | (std::intptr_t(makeCode) << 16) | (e0 ? (1 << 24) : 0) |
                   (up ? (std::intptr_t(1) << 30) | (std::intptr_t(1) << 31) : 0);
        e.modifiers = Mods();
        e.keyBits = KeyBitsNow();
        if (up) e.view = g_pageKey[dik].view;
        else g_pageKey[dik] = e;
        Push(e);
    };
    if (up) {   // an up goes where its down went
        const bool ours = g_taken[dik];
        if (g_pageDown[dik]) keyEvent();
        g_taken[dik] = g_pageDown[dik] = false;
        return ours;
    }
    if (repeat && !g_taken[dik]) return false;   // held for the game since before UI mode: still the game's
    if (repeat && !g_ui) return true;   // held through UI mode's close: ours to its up, which the game never sees
    // Alt+F4 still closes the game: raw input has no legacy WM_SYSKEYDOWN to let it through.
    if (vk == VK_F4 && g_alt) return false;

    // In UI mode a key that types is typing, even if a view is bound to it. Otherwise only the open view's own key
    // (closing it) and a HUD's act: another panel's key goes to the page (Magelight's invariant 15).
    const bool typing = g_ui && dik != kDikEscape && TextOf(vk, makeCode, false, false, false);
    if (!typing && !g_disabled) {
        for (const auto& r : views::Layout()) {
            if (!r.hotkey || r.hotkey != dik) continue;
            if (g_ui && r.id != g_ui && TakesUIMode(r)) break;
            if (g_blocked && TakesUIMode(r)) break;   // during a load a panel's key is the game's
            if (!repeat) Toggle(r);
            g_taken[dik] = true;
            return true;
        }
    }
    if (!g_ui) return false;
    g_taken[dik] = true;
    if (dik == kDikEscape) {
        if (!repeat) Close();
        return true;
    }
    keyEvent();
    g_pageDown[dik] = true;
    if (!g_sawChar) Text(vk, makeCode);
    return true;
}

bool OnMouse(int dx, int dy, unsigned short buttonFlags, short wheel) {
    if (GameButton(buttonFlags)) return false;
    if (!g_ui) return true;   // the up of a page's press, after UI mode closed
    // Counts scaled by screen height / 1080, in whole 1080ths so nothing is lost to rounding; the rest waits.
    g_fx += std::int64_t(dx) * g_sh;
    g_fy += std::int64_t(dy) * g_sh;
    const int mx = int(g_fx / 1080), my = int(g_fy / 1080);
    g_fx %= 1080;
    g_fy %= 1080;
    g_cx = std::clamp(g_cx + mx, 0, g_sw - 1);
    g_cy = std::clamp(g_cy + my, 0, g_sh - 1);
    return Pointer(mx || my, buttonFlags, wheel);
}

namespace {
bool Pointer(bool moved, unsigned short buttonFlags, short wheel) {
    const auto rows = views::Layout();
    const views::Row* held = g_capture ? ById(rows, g_capture) : nullptr;
    const views::Row* target = held ? held : Under(rows, g_cx, g_cy);
    const int over = target ? target->id : 0;
    if (const int left = g_hover.exchange(over); left && left != over) Leave(left);
    if (target && moved) Push(At(Event::Move, *target));
    const struct { USHORT down, up; int button; } buttons[] = {
        {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 1},
        {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 2},
        {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 3},
    };
    for (const auto& b : buttons) {
        if (!target) break;
        if (buttonFlags & b.down) {
            g_capture = target->id;
            Event e = At(Event::Down, *target);
            e.button = b.button;
            Push(e);
        }
        if (buttonFlags & b.up) {
            Event e = At(Event::Up, *target);
            e.button = b.button;
            Push(e);
            // The capture holds while any button is down (GameButton has already taken this one off).
            if (!(g_pageButtons & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN)))
                g_capture = 0;
        }
    }
    if (target && (buttonFlags & RI_MOUSE_WHEEL)) {
        Event e = At(Event::Wheel, *target);
        e.delta = wheel;
        Push(e);
    }
    return true;
}
}

bool OnChar(char32_t ch) {
    if (!g_ui) return false;
    if (ch >= 0xD800 && ch < 0xDC00) {   // the first half of a pair: wait for the second
        g_high = ch;
        return true;
    }
    if (ch >= 0xDC00 && ch < 0xE000) {
        if (!g_high) return true;
        ch = 0x10000 + ((g_high - 0xD800) << 10) + (ch - 0xDC00);
    }
    g_high = 0;
    if (!g_sawChar) {   // WM_CHAR does come: from now on text is its, and the key the raw path just typed is not twice
        g_sawChar = true;
        if (ch == g_lastTyped) return true;
    }
    if (ch >= 0x20) {
        Event c{Event::Char};
        c.view = g_ui;
        c.ch = ch;
        c.modifiers = Mods();
        c.keyBits = KeyBitsNow();
        Push(c);
    }
    return true;
}

void SetScreen(int width, int height) {
    g_sw = width;
    g_sh = height;
}

Cursor CursorNow() { return {g_ui != 0, g_cx, g_cy}; }
int UiModeView() { return g_ui; }

bool OpenUIMode(int id) {
    for (const auto& r : views::Layout()) {
        if (r.id != id) continue;
        if (!TakesUIMode(r) || g_disabled || g_blocked) return false;
        std::lock_guard lock(g_uiMutex);
        if (g_ui != id) Open(r);
        return true;
    }
    return false;
}

void CloseUIMode() { Close(); }

void OnFocusLost() { LoseFocus(); }

void Disable() { g_disabled = true; }
void SetLoading(bool loading) { g_blocked = loading; }
void AskClose() { g_closeAsked = true; }

void Tick() {
    if ((g_closeAsked.exchange(false) || g_disabled || g_blocked) && g_ui) Close();
}

void* Detour() { return reinterpret_cast<void*>(&Hooked); }
void** Original() { return reinterpret_cast<void**>(&g_original); }

void Drain(std::vector<Event>& out) {
    out.clear();
    std::lock_guard lock(g_mutex);
    out.swap(g_queue);
}
}
