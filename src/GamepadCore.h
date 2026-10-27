#pragma once
// The gamepad layer (1.6.0): who owns the pad, dead zones, repeat, and the held-button rules that keep a press from
// acting twice (on the page and in the game). Engine-free, so tools/gamepad-test runs it on the desktop; the input
// sink feeds it the engine's gamepad events on the game thread. Ported from Meridian UI's Meridian.Input/1, whose
// control, action and phase names pages see unchanged.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace Magelight::Gamepad {

    // Positional names: south is A on an Xbox pad and Cross on a PlayStation one. Numbered as the public API's
    // GamepadConfig bindings are.
    enum class Control : std::uint8_t {
        None, DpadUp, DpadDown, DpadLeft, DpadRight, Start, Back, LeftThumb, RightThumb, LeftShoulder,
        RightShoulder, South, East, West, North, LeftTrigger, RightTrigger, LeftStick, RightStick
    };
    inline constexpr std::size_t kControlCount = 19;

    // An action's binding slot is its value minus one.
    enum class Action : std::uint8_t {
        None, Up, Down, Left, Right, Accept, Cancel, PreviousTab, NextTab, Secondary, Tertiary, ToggleCursor
    };
    inline constexpr std::size_t kBindingCount = 11;

    enum class Phase : std::uint8_t { None, Press, Release, Change, Repeat, Cancel };
    enum class Style : std::uint8_t { Keys = 0, Navigation = 1 };
    enum class Mode : std::uint8_t { Navigation = 0, Cursor = 1 };
    enum class Glyphs : std::uint8_t { Xbox = 0, PlayStation = 1, Generic = 2 };

    inline constexpr std::size_t Index(Control c) { return static_cast<std::size_t>(c); }
    inline constexpr bool IsDigital(Control c) { return c >= Control::DpadUp && c <= Control::North; }

    inline const char* ControlName(Control c)
    {
        static constexpr const char* names[kControlCount] = {
            "none", "dpadUp", "dpadDown", "dpadLeft", "dpadRight", "start", "back", "leftThumb", "rightThumb",
            "leftShoulder", "rightShoulder", "south", "east", "west", "north", "leftTrigger", "rightTrigger",
            "leftStick", "rightStick"
        };
        return Index(c) < kControlCount ? names[Index(c)] : "none";
    }
    inline const char* ActionName(Action a)
    {
        static constexpr const char* names[kBindingCount + 1] = {
            "none", "up", "down", "left", "right", "accept", "cancel", "previousTab", "nextTab", "secondary",
            "tertiary", "toggleCursor"
        };
        const auto i = static_cast<std::size_t>(a);
        return i <= kBindingCount ? names[i] : "none";
    }
    inline const char* PhaseName(Phase p)
    {
        switch (p) {
        case Phase::Press: return "press";
        case Phase::Release: return "release";
        case Phase::Change: return "change";
        case Phase::Repeat: return "repeat";
        case Phase::Cancel: return "cancel";
        default: return "none";
        }
    }

    // SKSE's gamepad keycodes (SKSE::InputMap::GamepadMaskToKeycode): 266..281 are the sixteen digital controls,
    // the triggers included, in Control order.
    inline constexpr Control FromSkseKeycode(std::uint32_t id)
    {
        return id >= 266 && id <= 281 ? static_cast<Control>(id - 265) : Control::None;
    }
    // XInput button masks, and the engine's trigger ids 9 and 10, in Control order.
    inline constexpr Control FromXInputMask(std::uint32_t id)
    {
        constexpr std::uint32_t ids[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 4096, 8192, 16384, 32768, 9, 10 };
        for (std::size_t i = 0; i < 16; ++i)
            if (ids[i] == id) return static_cast<Control>(i + 1);
        return Control::None;
    }

    inline float ClampAxis(float v) { return std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f; }

    // Magelight.json "gamepad".
    struct Tuning {
        bool enabled = true;
        bool trace = false;
        float deadZone = 0.25f;
        float exitDeadZone = 0.18f;
        float cursorSpeed = 900.0f;      // reference px/s at a view height of 1080
        double repeatDelay = 0.35;
        double repeatInterval = 0.09;
        Glyphs glyphs = Glyphs::Xbox;
    };

    inline Tuning Clamped(Tuning t)
    {
        const auto fin = [](auto v, auto fallback) { return std::isfinite(v) ? v : fallback; };
        t.deadZone = std::clamp(fin(t.deadZone, 0.25f), 0.05f, 0.90f);
        // Below the entry threshold, or a stick at rest between the two would never leave the centre.
        t.exitDeadZone = std::clamp(fin(t.exitDeadZone, 0.18f), 0.0f, std::min(0.89f, t.deadZone - 0.01f));
        t.cursorSpeed = std::clamp(fin(t.cursorSpeed, 900.0f), 100.0f, 3000.0f);
        t.repeatDelay = std::clamp(fin(t.repeatDelay, 0.35), 0.1, 2.0);
        t.repeatInterval = std::clamp(fin(t.repeatInterval, 0.09), 0.03, 0.5);
        if (t.glyphs > Glyphs::Generic) t.glyphs = Glyphs::Xbox;
        return t;
    }

    inline constexpr std::array<Control, kBindingCount> DefaultBindings()
    {
        return { Control::DpadUp, Control::DpadDown, Control::DpadLeft, Control::DpadRight, Control::South,
                 Control::East, Control::LeftShoulder, Control::RightShoulder, Control::West, Control::North,
                 Control::RightThumb };
    }

    // Each action on one digital control at most, no control on two actions. Sticks and directions on the left
    // stick are not bindable: in navigation mode the left stick always navigates.
    inline bool ValidBindings(const std::array<Control, kBindingCount>& b)
    {
        for (std::size_t i = 0; i < kBindingCount; ++i) {
            if (b[i] == Control::None) continue;
            if (!IsDigital(b[i])) return false;
            for (std::size_t j = 0; j < i; ++j)
                if (b[j] == b[i]) return false;
        }
        return true;
    }

    struct ViewConfig {
        bool enabled = false;
        Style style = Style::Keys;
        bool allowCursor = false;
        bool startInCursor = false;
        std::array<Control, kBindingCount> bindings = DefaultBindings();
    };

    inline Action BindingAction(const ViewConfig& c, Control control)
    {
        if (control == Control::None) return Action::None;
        for (std::size_t i = 0; i < kBindingCount; ++i)
            if (c.bindings[i] == control) return static_cast<Action>(i + 1);
        return Action::None;
    }

    // The pad as XInput reports it (BSWin32GamepadDevice's current state), read to seed the ledger.
    struct PadSnapshot {
        std::uint16_t buttons = 0;
        std::uint8_t leftTrigger = 0, rightTrigger = 0;
        std::int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    };

    struct Transition {
        bool consume = false;    // the host unlinks the event: no later sink, and not the game, sees it
        Phase phase = Phase::None;
    };

    // Per-control physical state and who owns each press. A control that is off neutral when the owner changes is
    // blocked until it returns to neutral, so the button that opened or closed a page never acts in the next owner.
    // A release goes where its press went: to the game if the game saw the press, nowhere if a page owned it.
    class Ledger {
    public:
        Tuning tuning;

        bool SetOwner(std::uint64_t owner)
        {
            if (owner == m_owner) return false;
            const bool leaving = m_owner != 0;
            m_owner = owner;
            ++m_generation;
            m_repeatDirection = Control::None;
            for (std::size_t i = 1; i < kControlCount; ++i) {
                if (m_value[i] == 0.0f && m_y[i] == 0.0f) continue;
                m_blocked[i] = true;
                if (leaving) m_uiOwned[i] = true;
            }
            return true;
        }

        void RequireNeutral()
        {
            m_blocked.fill(true);
            m_repeatDirection = Control::None;
        }

        void Seed(const PadSnapshot& pad)
        {
            constexpr std::uint32_t masks[14] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 4096, 8192, 16384, 32768 };
            for (std::uint32_t m : masks) SeedOne(FromXInputMask(m), (pad.buttons & m) ? 1.0f : 0.0f);
            SeedOne(Control::LeftTrigger, pad.leftTrigger / 255.0f);
            SeedOne(Control::RightTrigger, pad.rightTrigger / 255.0f);
            SeedOne(Control::LeftStick, pad.lx / 32767.0f, pad.ly / 32767.0f);
            SeedOne(Control::RightStick, pad.rx / 32767.0f, pad.ry / 32767.0f);
        }

        void ResetDevice()
        {
            m_value.fill(0.0f);
            m_y.fill(0.0f);
            m_gameDown.fill(false);
            m_uiOwned.fill(false);
            RequireNeutral();
            ++m_generation;
        }

        void Capture(Control c)
        {
            if (Index(c) < kControlCount) m_blocked[Index(c)] = true;
        }

        bool Held(Control c) const { return Index(c) < kControlCount && m_value[Index(c)] > 0.0f; }
        bool Armed(Control c) const { return Index(c) < kControlCount && !m_blocked[Index(c)]; }
        float X(Control c) const { return Armed(c) ? m_value[Index(c)] : 0.0f; }
        float Y(Control c) const { return Armed(c) ? m_y[Index(c)] : 0.0f; }
        std::uint64_t Generation() const { return m_generation; }
        std::uint64_t Owner() const { return m_owner; }
        bool InitialRepeat() const { return m_initialRepeat; }

        // A digital control or a trigger. forceCapture consumes it and blocks it until release.
        Transition Button(Control c, float value, bool forceCapture = false)
        {
            if (c < Control::DpadUp || c > Control::RightTrigger) return {};
            const std::size_t i = Index(c);
            value = std::clamp(ClampAxis(value), 0.0f, 1.0f);
            if ((c == Control::LeftTrigger || c == Control::RightTrigger) && value < 0.10f) value = 0.0f;
            const float old = m_value[i];
            m_value[i] = value;
            const bool wasBlocked = m_blocked[i];
            bool consumed = m_owner != 0 || m_uiOwned[i] || forceCapture;
            if (value == 0.0f) {
                if (m_gameDown[i]) consumed = false;   // a press the game saw gets its release
                m_gameDown[i] = false;
                m_blocked[i] = false;
                m_uiOwned[i] = false;
            } else if (!consumed) {
                m_gameDown[i] = true;
            } else {
                m_uiOwned[i] = true;
            }
            if (forceCapture && value != 0.0f) m_blocked[i] = true;
            Phase phase = value == old ? Phase::None : value == 0.0f ? Phase::Release : old == 0.0f ? Phase::Press : Phase::Change;
            if (wasBlocked || forceCapture) phase = Phase::None;
            return { consumed, phase };
        }

        Transition Stick(Control c, float x, float y)
        {
            if (c != Control::LeftStick && c != Control::RightStick) return {};
            const std::size_t i = Index(c);
            x = ClampAxis(x);
            y = ClampAxis(y);
            const float threshold = (m_value[i] != 0.0f || m_y[i] != 0.0f) ? tuning.exitDeadZone : tuning.deadZone;
            if (std::hypot(x, y) < threshold) x = y = 0.0f;
            const bool changed = x != m_value[i] || y != m_y[i];
            m_value[i] = x;
            m_y[i] = y;
            const bool blocked = m_blocked[i];
            bool consume = m_owner != 0 || m_uiOwned[i];
            if (x == 0.0f && y == 0.0f) {
                if (m_gameDown[i]) consume = false;
                m_gameDown[i] = false;
                m_blocked[i] = false;
                m_uiOwned[i] = false;
            } else if (!consume) {
                m_gameDown[i] = true;
            } else {
                m_uiOwned[i] = true;
            }
            return { consume, changed && m_owner != 0 && !blocked ? Phase::Change : Phase::None };
        }

        // The direction held now, as the d-pad control that names it: the bound direction controls first, then the
        // left stick, whose axis in use keeps winning near the diagonal so it does not chatter between two.
        Control Direction(const ViewConfig* config) const
        {
            if (!m_owner) return Control::None;
            if (const Control d = BoundDirection(config); d != Control::None) return d;
            const float x = X(Control::LeftStick), y = Y(Control::LeftStick);
            if (x == 0.0f && y == 0.0f) return Control::None;
            const bool wasHorizontal = m_repeatDirection == Control::DpadLeft || m_repeatDirection == Control::DpadRight;
            const bool horizontal = std::fabs(x) > std::fabs(y) * (wasHorizontal ? 0.85f : 1.15f);
            return horizontal ? (x > 0.0f ? Control::DpadRight : Control::DpadLeft)
                              : (y > 0.0f ? Control::DpadUp : Control::DpadDown);
        }

        // The direction to emit now, or None: at once on a new direction, then after repeatDelay, then every
        // repeatInterval. includeStick false (cursor mode, where the left stick moves the cursor) counts only the
        // bound controls.
        Control Repeat(double now, bool includeStick, const ViewConfig* config)
        {
            const Control direction = includeStick ? Direction(config) : (m_owner ? BoundDirection(config) : Control::None);
            if (direction != m_repeatDirection) {
                m_initialRepeat = true;
                m_repeatDirection = direction;
                m_nextRepeat = now + tuning.repeatDelay;
                return direction;
            }
            if (direction != Control::None && now >= m_nextRepeat) {
                m_initialRepeat = false;
                m_nextRepeat = now + tuning.repeatInterval;
                return direction;
            }
            return Control::None;
        }

    private:
        void SeedOne(Control c, float x, float y = 0.0f)
        {
            if (c == Control::None || Index(c) >= kControlCount) return;
            const std::size_t i = Index(c);
            m_value[i] = ClampAxis(x);
            m_y[i] = ClampAxis(y);
            const bool stick = c == Control::LeftStick || c == Control::RightStick;
            const bool held = stick ? std::hypot(x, y) >= tuning.deadZone : x > 0.1f;
            m_blocked[i] = held;
            if (!held) m_value[i] = m_y[i] = 0.0f;
        }

        Control BoundDirection(const ViewConfig* config) const
        {
            for (std::size_t i = 0; i < 4; ++i) {
                const Control c = config ? config->bindings[i] : static_cast<Control>(i + 1);
                if (c != Control::None && Armed(c) && Held(c)) return static_cast<Control>(i + 1);
            }
            return Control::None;
        }

        std::uint64_t m_owner = 0, m_generation = 0;
        std::array<float, kControlCount> m_value{}, m_y{};
        std::array<bool, kControlCount> m_blocked{}, m_gameDown{}, m_uiOwned{};
        Control m_repeatDirection = Control::None;
        double m_nextRepeat = 0.0;
        bool m_initialRepeat = false;
    };

    // Windows virtual keys the keys style taps (kept here so the header needs no <windows.h>).
    inline constexpr int kVkReturn = 0x0D, kVkEscape = 0x1B, kVkLeft = 0x25, kVkUp = 0x26, kVkRight = 0x27, kVkDown = 0x28;

    inline constexpr std::size_t kMaxEvents = 128;

    struct Event {
        Control control = Control::None;
        Action action = Action::None;
        Phase phase = Phase::None;
        float value = 0.0f, x = 0.0f, y = 0.0f;
    };

    // One input batch as the host sees it. owner 0: no view owns the pad. A new ownerEpoch for the same owner (its
    // page reloaded) re-arms the held-button rules as a new owner would.
    struct BatchIn {
        double now = 0.0;                // seconds, any steady clock
        bool connected = false;
        std::uint64_t owner = 0;
        std::uint64_t ownerEpoch = 0;
        ViewConfig config;               // the owner's
        Mode mode = Mode::Navigation;    // the owner's, as the last batch left it
        bool escapeCaptured = false;     // the owner's page took Escape (SetEscapeCapture / __escapecapture)
        PadSnapshot pad;                 // read when the ledger is seeded: a connect, an owner change, an overflow
    };

    struct PointerOut {
        float dx = 0.0f, dy = 0.0f;      // reference px; the host scales by the view's height / 1080
        std::vector<bool> buttons;       // left button: true down, false up, in order
        bool release = false;            // release a left press the cursor still holds
    };

    struct BatchOut {
        std::uint64_t generation = 0;
        bool reset = false;              // the page cancels held custom input (owner change, overflow, reload)
        bool stateChanged = false;       // mode, device or connection changed: worth a packet without events
        Mode mode = Mode::Navigation;
        bool gamepadDevice = false;      // the pad, not the mouse and keyboard, is in use
        double dt = 0.0;
        std::vector<Event> events;       // navigation style
        std::vector<int> keys;           // keys style: virtual keys to tap, in order
        PointerOut pointer;              // cursor mode
    };

    // Turns one batch of engine gamepad events into what the owning page gets. Begin, then Button / Stick for each
    // event in the batch (true: the host unlinks it), then End. Game thread only.
    class Router {
    public:
        Tuning tuning;

        void Begin(const BatchIn& in)
        {
            m_ledger.tuning = tuning;
            m_in = in;
            m_out = {};
            if (in.connected != m_connected) {
                m_connected = in.connected;
                m_ledger.ResetDevice();
                if (m_connected) m_ledger.Seed(in.pad);
                m_out.stateChanged = true;
            }
            const std::uint64_t owner = (m_connected && tuning.enabled && in.config.enabled) ? in.owner : 0;
            const std::uint64_t previous = m_ledger.Owner();
            if (owner && owner == previous && in.ownerEpoch != m_epoch) {
                m_ledger.SetOwner(0);
                m_ledger.Seed(in.pad);
            }
            m_epoch = in.ownerEpoch;
            m_out.reset = m_ledger.SetOwner(owner);
            if (m_out.reset) {
                m_out.stateChanged = true;
                ReleaseCursorPress();
            }
            m_mode = in.config.allowCursor ? in.mode : Mode::Navigation;
            m_dt = m_haveTime ? std::clamp(in.now - m_lastTime, 0.0, 0.05) : 0.0;
            m_lastTime = in.now;
            m_haveTime = true;
        }

        bool Button(Control c, float value)
        {
            const bool active = m_ledger.Owner() != 0;
            if (c == Control::None) return active;   // a pad control we do not name stays out of the game too
            const Transition t = m_ledger.Button(c, value);
            if (t.phase == Phase::Press) NoteGamepadDevice();
            if (!active || t.phase == Phase::None) return t.consume;
            const ViewConfig& cfg = m_in.config;
            const Action action = BindingAction(cfg, c);
            // B (and cancel) belongs to the page only while it holds Escape; otherwise the focus menu's Cancel
            // closes the view, and the page must not also see it.
            if ((c == Control::East || action == Action::Cancel) && !m_in.escapeCaptured) return t.consume;
            if (action >= Action::Up && action <= Action::Right) {
                // Presses and repeats come from the clock in End, so a held direction emits once per batch.
                if (t.phase == Phase::Release && cfg.style == Style::Navigation) Emit({ c, action, Phase::Release });
                if (t.phase == Phase::Press && m_mode == Mode::Cursor) SetMode(Mode::Navigation);
                return t.consume;
            }
            if (action == Action::ToggleCursor && t.phase == Phase::Press && cfg.allowCursor) {
                SetMode(m_mode == Mode::Navigation ? Mode::Cursor : Mode::Navigation);
                // An accept already down when the mode flips must not click (or accept) when it is let go.
                const Control accept = cfg.bindings[static_cast<std::size_t>(Action::Accept) - 1];
                if (m_ledger.Held(accept)) m_ledger.Capture(accept);
                return t.consume;
            }
            if (m_mode == Mode::Cursor && action == Action::Accept) {
                if (t.phase == Phase::Press) { m_out.pointer.buttons.push_back(true); m_cursorDown = true; }
                if (t.phase == Phase::Release) { m_out.pointer.buttons.push_back(false); m_cursorDown = false; }
                return t.consume;
            }
            if (cfg.style == Style::Navigation) {
                Emit({ c, action, t.phase, value, value, 0.0f });
            } else if (t.phase == Phase::Press) {
                if (action == Action::Accept) m_out.keys.push_back(kVkReturn);
                else if (action == Action::Cancel) m_out.keys.push_back(kVkEscape);
            }
            return t.consume;
        }

        bool Stick(Control c, float x, float y)
        {
            const Transition t = m_ledger.Stick(c, x, y);
            if (t.phase != Phase::None) {
                m_axesChanged = true;
                if (m_ledger.X(c) != 0.0f || m_ledger.Y(c) != 0.0f) NoteGamepadDevice();
            }
            return t.consume;
        }

        // The mouse moved or a key was pressed: the mouse and keyboard are the device again.
        void OtherDeviceActivity()
        {
            if (!m_gamepadDevice) return;
            m_gamepadDevice = false;
            m_out.stateChanged = true;
            m_out.pointer.buttons.clear();
            ReleaseCursorPress();
        }

        BatchOut End()
        {
            const ViewConfig& cfg = m_in.config;
            if (m_ledger.Owner() != 0) {
                const Control dir = m_ledger.Repeat(m_in.now, m_mode == Mode::Navigation, &cfg);
                if (dir != Control::None) {
                    const auto action = static_cast<Action>(Index(dir));
                    const Control bound = cfg.bindings[Index(dir) - 1];
                    const Control source = m_ledger.Held(bound) ? bound : Control::LeftStick;
                    if (cfg.style == Style::Navigation)
                        Emit({ source, action, m_ledger.InitialRepeat() ? Phase::Press : Phase::Repeat, 1.0f });
                    else
                        m_out.keys.push_back(ArrowKey(action));
                }
                if (cfg.style == Style::Navigation) {
                    for (const Control c : { Control::LeftStick, Control::RightStick }) {
                        const float x = m_ledger.X(c), y = m_ledger.Y(c);
                        if (m_axesChanged || x != 0.0f || y != 0.0f) Emit({ c, Action::None, Phase::Change, x, x, y });
                    }
                }
                if (m_mode == Mode::Cursor && m_gamepadDevice) {
                    const auto step = static_cast<float>(tuning.cursorSpeed * m_dt);
                    m_out.pointer.dx = m_ledger.X(Control::LeftStick) * step;
                    m_out.pointer.dy = -m_ledger.Y(Control::LeftStick) * step;
                }
            }
            if (m_out.events.size() + m_out.keys.size() > kMaxEvents) {
                m_out.events.clear();
                m_out.keys.clear();
                m_ledger.RequireNeutral();
                m_ledger.Seed(m_in.pad);
                m_out.pointer = {};
                ReleaseCursorPress();
                m_out.reset = true;
            }
            m_axesChanged = false;
            m_out.generation = m_ledger.Generation();
            m_out.mode = m_mode;
            m_out.gamepadDevice = m_gamepadDevice;
            m_out.dt = m_dt;
            return std::move(m_out);
        }

    private:
        static int ArrowKey(Action a)
        {
            switch (a) {
            case Action::Up: return kVkUp;
            case Action::Down: return kVkDown;
            case Action::Left: return kVkLeft;
            default: return kVkRight;
            }
        }
        void Emit(const Event& e) { m_out.events.push_back(e); }
        void NoteGamepadDevice()
        {
            if (m_gamepadDevice) return;
            m_gamepadDevice = true;
            m_out.stateChanged = true;
        }
        void SetMode(Mode m)
        {
            m_mode = m;
            m_out.stateChanged = true;
            m_out.pointer.buttons.clear();
            m_out.pointer.release = true;
            m_cursorDown = false;
        }
        void ReleaseCursorPress()
        {
            if (!m_cursorDown) return;
            m_cursorDown = false;
            m_out.pointer.release = true;
        }

        Ledger m_ledger;
        BatchIn m_in;
        BatchOut m_out;
        Mode m_mode = Mode::Navigation;
        bool m_connected = false;
        bool m_gamepadDevice = false;
        bool m_cursorDown = false;
        bool m_axesChanged = false;
        bool m_haveTime = false;
        std::uint64_t m_epoch = 0;
        double m_lastTime = 0.0, m_dt = 0.0;
    };

    // A page id the helper made up: 1 to 80 letters, digits, '-' or '_'. Packets embed it unescaped because of this.
    inline bool ValidPageId(std::string_view id)
    {
        if (id.empty() || id.size() > 80) return false;
        for (const char ch : id) {
            const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                            ch == '-' || ch == '_';
            if (!ok) return false;
        }
        return true;
    }

    struct PacketIn {
        std::string_view page;           // ValidPageId
        std::uint64_t generation = 0, sequence = 0;
        bool active = false, reset = false, enabled = false, connected = false;
        Style style = Style::Keys;
        Mode mode = Mode::Navigation;
        bool gamepadDevice = false;
        Glyphs glyphs = Glyphs::Xbox;
        const ViewConfig* config = nullptr;
        double dt = 0.0;
        const std::vector<Event>* events = nullptr;
    };

    // The '__gamepad' message the page core's helper reads: Meridian.Input/1's packet plus the view's style.
    inline std::string PacketJson(const PacketIn& p)
    {
        const auto num = [](double v) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.6g", std::isfinite(v) ? v : 0.0);
            return std::string(buf);
        };
        const auto flag = [](bool b) { return b ? "true" : "false"; };
        static constexpr const char* glyphNames[] = { "xbox", "playstation", "generic" };
        std::string s;
        s.reserve(512);
        s += "{\"version\":1,\"page\":\"";
        s += p.page;
        s += "\",\"generation\":" + std::to_string(p.generation) + ",\"sequence\":" + std::to_string(p.sequence);
        s += std::string(",\"active\":") + flag(p.active) + ",\"reset\":" + flag(p.reset) + ",\"enabled\":" +
             flag(p.enabled) + ",\"connected\":" + flag(p.connected);
        s += std::string(",\"style\":\"") + (p.style == Style::Navigation ? "navigation" : "keys") + "\"";
        s += std::string(",\"mode\":\"") + (p.mode == Mode::Cursor ? "cursor" : "navigation") + "\"";
        s += std::string(",\"device\":\"") + (p.gamepadDevice ? "gamepad" : "keyboardMouse") + "\"";
        s += std::string(",\"glyphFamily\":\"") + glyphNames[std::min<std::size_t>(static_cast<std::size_t>(p.glyphs), 2)] + "\"";
        s += ",\"bindings\":{";
        const ViewConfig cfg = p.config ? *p.config : ViewConfig{};
        for (std::size_t i = 0; i < kBindingCount; ++i) {
            if (i) s += ',';
            s += std::string("\"") + ActionName(static_cast<Action>(i + 1)) + "\":\"" + ControlName(cfg.bindings[i]) + "\"";
        }
        s += "},\"dt\":" + num(std::clamp(p.dt, 0.0, 0.05)) + ",\"events\":[";
        if (p.events) {
            for (std::size_t i = 0; i < p.events->size(); ++i) {
                const Event& e = (*p.events)[i];
                if (i) s += ',';
                s += std::string("{\"control\":\"") + ControlName(e.control) + "\",\"action\":\"" + ActionName(e.action) +
                     "\",\"phase\":\"" + PhaseName(e.phase) + "\",\"value\":" + num(e.value) + ",\"x\":" + num(e.x) +
                     ",\"y\":" + num(e.y) + "}";
            }
        }
        s += "]}";
        return s;
    }

}  // namespace Magelight::Gamepad
