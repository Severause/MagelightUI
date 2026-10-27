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

}  // namespace Magelight::Gamepad
