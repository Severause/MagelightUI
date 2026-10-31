#pragma once
// Input the host hands Ultralight 2.0.
//
// Keys: the Windows KeyEvent constructor gives the page KeyboardEvent.code, a key that follows the keyboard layout
// ("@" for AltGr+Q, "Dead") and location from a message's wParam and lParam, so every key the host fires carries the
// lParam Windows would have sent. It reads the modifier and lock state from the calling thread's keyboard state while
// it runs, not when the event is fired, and once it has built its platform event it ignores the public modifiers and
// is_auto_repeat fields. The host fires keys a frame after they were queued, and on VR the window thread never sees a
// key message (its state is all up), so each key carries the state it was queued with (KeyBits) and MakeKeyEvent
// applies that state around the constructor alone.
//
// Pointer: the page core's guard for the buttons other than the left one (kPointerGuardScript), and which view each
// pointer source's events go to (PointerRoutes).
//
// Polled keys: which module the host's GetKeyState / GetAsyncKeyState filter hides keys from (IsForeignPluginPath).
//
// Windows and Ultralight only: tools/input-test runs all of it on the desktop.

#include <windows.h>

#include <Ultralight/KeyEvent.h>

#include <cstdint>
#include <cstring>
#include <string_view>

namespace Magelight::Input {

    // The state a key message is read with: the side of each modifier, then the two lock toggles.
    enum KeyBits : std::uint16_t {
        kKbLShift = 1, kKbRShift = 2, kKbLCtrl = 4, kKbRCtrl = 8, kKbLAlt = 16, kKbRAlt = 32, kKbLWin = 64, kKbRWin = 128,
        kKbCaps = 256, kKbNumLock = 512,
        kKbModifiers = 0xFF,
    };

    // The side-specific virtual key of each modifier bit, in bit order (kKbLShift first). GetKeyState and
    // GetAsyncKeyState answer for these.
    inline constexpr int kModifierVk[8] = { VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
                                            VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN };
    // The virtual key the window sends for each of them: side-neutral for Shift, Ctrl and Alt, as WM_KEYDOWN has them.
    inline constexpr int kModifierMsgVk[8] = { VK_SHIFT, VK_SHIFT, VK_CONTROL, VK_CONTROL,
                                               VK_MENU, VK_MENU, VK_LWIN, VK_RWIN };

    // Keys Windows sends with lParam bit 24 (the E0 prefix). MapVirtualKeyW gives none of them the prefix
    // (MAPVK_VK_TO_VSC_EX gives VK_LEFT 0x4B too), and without it the constructor reads a navigation key as the
    // keypad key on the same scancode: VK_LEFT arrives as code "Numpad4", location 3.
    inline bool IsExtendedVk(UINT vk)
    {
        switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_DIVIDE: case VK_NUMLOCK: case VK_RCONTROL: case VK_RMENU:
        case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_SNAPSHOT:
            return true;
        default:
            return false;
        }
    }

    // The lParam the window would send with a key the host makes up from a bare virtual key (the VR keyboards,
    // the sink's Escape, Magelight_DeliverVKey): repeat count 1, the scancode of the calling thread's layout, the
    // extended bit, and for a release the previous-state and transition bits. A side-specific modifier (VK_RSHIFT)
    // gets that side's scancode.
    inline LPARAM SyntheticKeyLParam(UINT vk, bool up)
    {
        const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) & 0xFF;
        LPARAM l = 1 | (static_cast<LPARAM>(scan) << 16) | (IsExtendedVk(vk) ? (static_cast<LPARAM>(1) << 24) : 0);
        if (up) l |= (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
        return l;
    }

    // The public modifier flags `bits` describe, the Windows keys as metaKey. The host builds one KeyEvent field by
    // field (the Char of a surrogate pair), and passes it the bits without the Windows keys.
    inline unsigned ModsFromKeyBits(std::uint16_t bits)
    {
        unsigned m = 0;
        if (bits & (kKbLAlt | kKbRAlt)) m |= ultralight::KeyEvent::kMod_AltKey;
        if (bits & (kKbLCtrl | kKbRCtrl)) m |= ultralight::KeyEvent::kMod_CtrlKey;
        if (bits & (kKbLWin | kKbRWin)) m |= ultralight::KeyEvent::kMod_MetaKey;
        if (bits & (kKbLShift | kKbRShift)) m |= ultralight::KeyEvent::kMod_ShiftKey;
        return m;
    }

    // Writes `bits` into a keyboard state array: each modifier's two sides and its side-neutral key, and the two
    // lock toggles. The down bit of every other key, and the locks' down bits, are left as they were.
    inline void ApplyKeyBits(BYTE (&ks)[256], std::uint16_t bits)
    {
        auto set = [&](int vk, unsigned mask) {
            ks[vk] = static_cast<BYTE>((ks[vk] & 1) | ((bits & mask) != 0 ? 0x80 : 0));
        };
        set(VK_LSHIFT, kKbLShift); set(VK_RSHIFT, kKbRShift); set(VK_SHIFT, kKbLShift | kKbRShift);
        set(VK_LCONTROL, kKbLCtrl); set(VK_RCONTROL, kKbRCtrl); set(VK_CONTROL, kKbLCtrl | kKbRCtrl);
        set(VK_LMENU, kKbLAlt); set(VK_RMENU, kKbRAlt); set(VK_MENU, kKbLAlt | kKbRAlt);
        set(VK_LWIN, kKbLWin); set(VK_RWIN, kKbRWin);
        ks[VK_CAPITAL] = static_cast<BYTE>((ks[VK_CAPITAL] & 0x80) | ((bits & kKbCaps) ? 1 : 0));
        ks[VK_NUMLOCK] = static_cast<BYTE>((ks[VK_NUMLOCK] & 0x80) | ((bits & kKbNumLock) ? 1 : 0));
    }

    // Every key event the host fires is made here. The thread's keyboard state is set to `bits` for the constructor
    // alone and put back before anything else runs (the dead key a layout holds is not touched). The constructor, in
    // WebCore (which the polled-keys filter exempts by its folder), reads Shift, Ctrl, Alt and Caps Lock with
    // GetKeyState and the whole state with GetKeyboardState, and no Windows key, so a page's metaKey is false. Nothing
    // may read the event's public fields afterwards: modifiers, key_identifier and native_key_code are left unfilled.
    // A key-up drops lParam bit 30, which the constructor would report as a repeat. Ultralight's thread (the
    // constructor is an Ultralight call). `stateApplied`, when given, says whether `bits` reached the constructor:
    // false only when the thread's state could not be read, and then the event has the thread's own state.
    inline ultralight::KeyEvent MakeKeyEvent(ultralight::KeyEvent::Type type, WPARAM w, LPARAM l, bool sys,
                                             std::uint16_t bits, bool* stateApplied = nullptr)
    {
        BYTE saved[256]{};
        const bool have = GetKeyboardState(saved) != FALSE;
        if (have) {
            BYTE ks[256];
            std::memcpy(ks, saved, sizeof ks);
            ApplyKeyBits(ks, bits);
            SetKeyboardState(ks);
        }
        if (type == ultralight::KeyEvent::kType_KeyUp) l &= ~(static_cast<LPARAM>(1) << 30);
        ultralight::KeyEvent evt(type, w, l, sys);
        if (have) SetKeyboardState(saved);
        if (stateApplied) *stateApplied = have;
        return evt;
    }

    // A DirectInput scancode as the window's key message would carry it (VR types from the engine's keyboard device).
    // DirectInput puts an E0-prefixed key at its make code + 0x80 (DIK_UP 0xC8 is E0 48), which MapVirtualKeyW does
    // not know: it gets the E0 form and the message gets lParam bit 24. Pause (DIK 0xC5) is E1 1D 45, not E0 45
    // (NumLock or nothing to MapVirtualKeyW), so it is mapped by hand. The window delivers the side-neutral
    // VK_SHIFT, VK_CONTROL and VK_MENU, and VK_NUMPAD0-9 / VK_DECIMAL for the keypad with NumLock on; MapVirtualKeyW
    // gives the side-specific codes and the navigation keys, so both are folded here. lparam holds the scancode and
    // the extended bit only (no repeat count or transition bits). vk 0 = no such key.
    struct ScancodeKey {
        UINT vk = 0;
        LPARAM lparam = 0;
        std::uint32_t make = 0;   // the scancode without the E0 marker, as ToUnicode takes it
    };
    inline ScancodeKey FromScancode(std::uint32_t scan, bool numLock)
    {
        ScancodeKey k;
        if (scan == 0 || scan > 0xFF) return k;
        constexpr std::uint32_t kDikPause = 0xC5;
        const bool extended = scan >= 0x80 && scan != kDikPause;
        const std::uint32_t make = scan & 0x7F;
        UINT vk = scan == kDikPause ? VK_PAUSE : MapVirtualKeyW(extended ? (0xE000u | make) : scan, MAPVK_VSC_TO_VK_EX);
        if (!vk) return k;
        switch (vk) {
        case VK_LSHIFT:   case VK_RSHIFT:   vk = VK_SHIFT;   break;
        case VK_LCONTROL: case VK_RCONTROL: vk = VK_CONTROL; break;
        case VK_LMENU:    case VK_RMENU:    vk = VK_MENU;    break;
        default: break;
        }
        if (!extended && make >= 0x47 && make <= 0x53 && numLock) {
            // Keypad 7 8 9 - 4 5 6 + 1 2 3 0 . ; minus and plus keep their own codes.
            static constexpr UINT kPad[13] = { VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, 0, VK_NUMPAD4, VK_NUMPAD5,
                VK_NUMPAD6, 0, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD0, VK_DECIMAL };
            if (const UINT pad = kPad[make - 0x47]) vk = pad;
        }
        k.vk = vk;
        k.make = make;
        k.lparam = (static_cast<LPARAM>(make) << 16) | (extended ? (static_cast<LPARAM>(1) << 24) : 0);
        return k;
    }

    // Keys DirectInput saw that the window never got. On some systems a page in UI mode gets no key message until the
    // player alt-tabs, and the muted keyboard poll is then the only thing that sees the keys. The poll notes every key
    // here and passes on what falls due. One key press through the window proves the window hears keys, so nothing is
    // passed on for the rest of the session: a session that works never gets a key twice. A key waits kWaitPolls polls
    // and kWaitMs first, because a press can reach the device a frame ahead of its message. Alt, the Windows keys, what
    // is pressed with them held (Alt+Tab) and Print Screen, which never makes a key press, stay with the system. One
    // thread.
    class LostKeys {
    public:
        static constexpr int kWaitPolls = 3;
        static constexpr std::uint64_t kWaitMs = 60;
        static constexpr std::size_t kCap = 32;

        // A new UI-mode session. windowPresses: the host's count of key presses the window has had.
        void Begin(std::uint64_t windowPresses) { *this = LostKeys{}; base_ = windowPresses; }

        // key: the DirectInput scancode.
        void Note(std::uint8_t key, bool down, std::uint64_t nowMs)
        {
            if (key == 0x38 || key == 0xB8) { alt_ = down; return; }
            if (key == 0xDB || key == 0xDC) { win_ = down; return; }
            if (key == 0xB7) return;
            if (heard_ || count_ == kCap || (down && (alt_ || win_))) return;
            queue_[(head_ + count_++) % kCap] = { nowMs, 0, key, down };
        }

        // Once a poll, after the Notes. forward(key, down) sends one key and returns whether it went; a key whose
        // press went gets its release, and the releases still owed go out when the window starts hearing keys.
        template <class F>
        void Poll(std::uint64_t windowPresses, std::uint64_t nowMs, F&& forward)
        {
            if (heard_) return;
            if (windowPresses != base_) {
                heard_ = true;
                count_ = 0;
                for (int k = 0; k < 256; ++k)
                    if (held_[k]) { held_[k] = false; forward(static_cast<std::uint8_t>(k), false); }
                return;
            }
            for (std::size_t i = 0; i < count_; ++i) ++queue_[(head_ + i) % kCap].polls;
            while (count_) {
                const Entry& e = queue_[head_];
                if (e.polls < kWaitPolls || nowMs - e.ms < kWaitMs) break;
                if (e.down) {
                    if (forward(e.key, true)) { held_[e.key] = true; ++passed_; }
                } else if (held_[e.key]) {
                    held_[e.key] = false;
                    forward(e.key, false);
                }
                head_ = (head_ + 1) % kCap;
                --count_;
            }
        }

        bool WindowHeard() const { return heard_; }
        int Passed() const { return passed_; }   // key presses passed on this session

    private:
        struct Entry { std::uint64_t ms = 0; int polls = 0; std::uint8_t key = 0; bool down = false; };
        Entry queue_[kCap]{};
        std::size_t head_ = 0, count_ = 0;
        std::uint64_t base_ = 0;
        bool held_[256]{};
        bool heard_ = false, alt_ = false, win_ = false;
        int passed_ = 0;
    };

    // The polled-keys filter's test of a module's file path, lower case: another mod's SKSE plugin, a DLL under
    // SKSE\Plugins outside Magelight's runtime folder. By folder name, not the runtime path the host knows: under MO2
    // a module's file name is its real path in the mod's folder, while the host's is the virtual Data path. WebCore,
    // in that folder, reads the key state MakeKeyEvent sets, so it must never be filtered.
    inline bool IsForeignPluginPath(std::wstring_view lowerPath)
    {
        const bool ours = lowerPath.find(L"\\skse\\plugins\\magelight\\") != std::wstring_view::npos;
        return !ours && lowerPath.find(L"\\skse\\plugins\\") != std::wstring_view::npos;
    }

    // Part of the page core (local main frames). A middle press starts WebKit's autoscroll, which then scrolls on
    // every move with no button held and eats the next press; preventDefault on the middle mousedown stops it
    // (Ultralight 1.4 and 2.0). Ultralight 2.0 fires `click` for the side buttons, and for the middle one once its
    // autoscroll is stopped, so they run onclick handlers and follow links (a middle click follows a link even with
    // autoscroll), and it fires no `auxclick`: a trusted non-primary click is stopped and dispatched again on its target
    // as `auxclick`, as a browser sends it. A second press of the same button within the double-click time also fires
    // `dblclick`, even after its click was stopped, and a browser sends one for the primary button only, so a trusted
    // non-primary dblclick is stopped too. Listeners on window in the capture phase, added before any page script, run
    // first. tools/input-test checks each part.
    inline constexpr const char* kPointerGuardScript =
        "(function(){window.addEventListener('mousedown',function(e){if(e.button===1&&e.isTrusted)e.preventDefault();},true);"
        "window.addEventListener('click',function(e){if(e.button!==0&&e.isTrusted){e.stopImmediatePropagation();"
        "e.preventDefault();try{e.target.dispatchEvent(new MouseEvent('auxclick',e));}catch(x){}}},true);"
        "window.addEventListener('dblclick',function(e){if(e.button!==0&&e.isTrusted){e.stopImmediatePropagation();"
        "e.preventDefault();}},true);})();";

    // The input queue (QueueInput) between the threads that make input and the render thread, which drains it once a
    // frame, or once a page frame with pageThread, where a page's script can make one seconds long while the mouse
    // keeps moving. A move replaces a move of the same pointer source still at the tail (the page needs where the
    // pointer is, as a browser merges its mousemoves), and past kQueueSoftCap only what ends something still goes in,
    // up to kQueueHardCap: a release, and the host's markers (`marker`: the focus change, the release after the window
    // lost focus). Dropping those once the queue filled left a page that came back holding a press, a modifier and key
    // focus outside UI mode. tailIsMoveOfSource: the last message queued is a WM_MOUSEMOVE of this message's source.
    inline constexpr std::size_t kQueueSoftCap = 512, kQueueHardCap = 4096;
    enum class QueueStep { Append, ReplaceTail, Drop };
    inline QueueStep QueueRule(std::size_t queued, bool tailIsMoveOfSource, UINT msg, bool marker)
    {
        if (msg == WM_MOUSEMOVE && tailIsMoveOfSource) return QueueStep::ReplaceTail;
        if (queued < kQueueSoftCap) return QueueStep::Append;
        const bool ends = marker || msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP ||
                          msg == WM_XBUTTONUP || msg == WM_KEYUP || msg == WM_SYSKEYUP;
        return ends && queued < kQueueHardCap ? QueueStep::Append : QueueStep::Drop;
    }

    // Pointer routing over view ids, for DrainInputQueue (render thread). Each pointer source has its own press and
    // hover: the flat mouse, and each VR hand, whose ray points on its own. A press keeps its view for that source's
    // moves and releases until all of the source's buttons are up, as in a browser; another source's events go where
    // they point, so one hand's moves never reach the view the other holds a press on (a held key on the keyboard
    // page would see its mouseleave and stop repeating) and never send the view the other points at a leave.
    // Engine-free: tools/input-test drives it with two sources.
    enum PointerSource : std::uint8_t { kPointerMouse = 0, kPointerHand0 = 1, kPointerHand1 = 2, kPointerSources = 3 };

    class PointerRoutes {
    public:
        // The view `src`'s held buttons were pressed on; 0 when it holds none.
        std::uint64_t Captured(unsigned src) const { return m_src[Index(src)].view; }
        // `src`'s pressed view lost its page: the source holds nothing.
        void Drop(unsigned src) { m_src[Index(src)] = Src{ m_src[Index(src)].hover }; }

        // `src`'s pointer went to `view` (0 = none). The view it left loses its tooltip once no source points at it,
        // and gets its leave (a move outside itself, clearing its :hover) once no source holds a press there either.
        struct Left { std::uint64_t tooltip = 0, leave = 0; };
        Left Hover(unsigned src, std::uint64_t view)
        {
            const unsigned i = Index(src);
            const std::uint64_t prev = m_src[i].hover;
            if (prev == view) return {};
            m_src[i].hover = view;
            Left out;
            if (!prev) return out;
            for (unsigned o = 0; o < kPointerSources; ++o)
                if (o != i && m_src[o].hover == prev) return out;
            out.tooltip = prev;
            for (const Src& o : m_src)
                if (o.view == prev) return out;
            out.leave = prev;
            return out;
        }

        // `button` went down on `view` at (x, y), the position that view was sent.
        void Press(unsigned src, std::uint64_t view, int button, int x, int y)
        {
            Src& s = m_src[Index(src)];
            s.view = view;
            s.mask |= 1u << button;
            s.x = x;
            s.y = y;
        }
        // A move went to `view`: the release a lost hold gets lands at the last position its view was sent.
        void Moved(unsigned src, std::uint64_t view, int x, int y)
        {
            Src& s = m_src[Index(src)];
            if (s.view && s.view == view) { s.x = x; s.y = y; }
        }
        // `button` came up; `view` is where its release went (0 = nowhere).
        void Release(unsigned src, std::uint64_t view, int button, int x, int y)
        {
            Src& s = m_src[Index(src)];
            Moved(src, view, x, y);
            s.mask &= ~(1u << button);
            if (!s.mask) s.view = 0;
        }

        // Takes the buttons `src` holds, for releases no message will bring (focus lost, UI mode left).
        struct Held { std::uint64_t view = 0; unsigned mask = 0; int x = 0, y = 0; };
        Held TakeHeld(unsigned src)
        {
            Src& s = m_src[Index(src)];
            const Held h{ s.view, s.view ? s.mask : 0u, s.x, s.y };
            s = Src{ s.hover };
            return h;
        }

    private:
        struct Src { std::uint64_t hover = 0, view = 0; unsigned mask = 0; int x = 0, y = 0; };   // view != 0 iff mask != 0
        static unsigned Index(unsigned src) { return src < kPointerSources ? src : kPointerMouse; }
        Src m_src[kPointerSources];
    };
}
