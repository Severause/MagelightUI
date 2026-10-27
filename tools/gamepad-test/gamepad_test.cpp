// The gamepad layer's rules in src/GamepadCore.h (1.6.0), on the desktop: build.bat builds and runs this; exit 0
// when every check passes.

#include "GamepadCore.h"

#include <cmath>
#include <cstdio>
#include <string_view>

using namespace Magelight::Gamepad;

static int s_failed = 0;

static void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++s_failed;
}

static bool Near(double a, double b, double eps = 1e-5) { return std::fabs(a - b) <= eps; }
static bool Is(const char* a, std::string_view b) { return a && std::string_view(a) == b; }

static std::size_t Slot(Action a) { return static_cast<std::size_t>(a) - 1; }

int main()
{
    // names_match_meridian: the strings pages see are Meridian.Input/1's.
    {
        Check(Is(ControlName(Control::RightStick), "rightStick"), "ControlName(RightStick) is rightStick");
        Check(Is(ControlName(Control::South), "south"), "ControlName(South) is south");
        Check(Is(ActionName(Action::ToggleCursor), "toggleCursor"), "ActionName(ToggleCursor) is toggleCursor");
        Check(Is(PhaseName(Phase::Repeat), "repeat"), "PhaseName(Repeat) is repeat");
        Check(FromSkseKeycode(266) == Control::DpadUp, "SKSE keycode 266 is the d-pad up");
        Check(FromSkseKeycode(281) == Control::RightTrigger, "SKSE keycode 281 is the right trigger");
        Check(FromSkseKeycode(265) == Control::None, "SKSE keycode 265 is no pad control");
        Check(FromXInputMask(4096) == Control::South, "XInput mask 4096 is south (A)");
        Check(FromXInputMask(512) == Control::RightShoulder, "XInput mask 512 is the right shoulder");
    }

    // tuning_clamped: Magelight.json values outside the ranges come back inside them.
    {
        Tuning t;
        t.deadZone = 2.0f;
        Check(Near(Clamped(t).deadZone, 0.90), "a dead zone of 2 is clamped to 0.90");
        Tuning e;
        e.exitDeadZone = 0.95f;
        Check(Clamped(e).exitDeadZone < Clamped(e).deadZone, "the exit dead zone stays below the dead zone");
        Tuning r;
        r.repeatInterval = 0.0;
        Check(Near(Clamped(r).repeatInterval, 0.03), "a repeat interval of 0 is clamped to 0.03");
        Tuning c;
        c.cursorSpeed = 50.0f;
        Check(Near(Clamped(c).cursorSpeed, 100.0), "a cursor speed of 50 is clamped to 100");
        Check(Near(Clamped(Tuning{}).deadZone, 0.25) && Near(Clamped(Tuning{}).repeatDelay, 0.35),
              "the defaults are inside the ranges and unchanged");
    }

    // bindings_validated: unique digital controls or None.
    {
        Check(ValidBindings(DefaultBindings()), "the default bindings are valid");
        auto twice = DefaultBindings();
        twice[Slot(Action::Cancel)] = Control::South;
        Check(!ValidBindings(twice), "south bound to accept and cancel is refused");
        auto stick = DefaultBindings();
        stick[Slot(Action::Up)] = Control::LeftStick;
        Check(!ValidBindings(stick), "a stick as a binding is refused");
        auto none = DefaultBindings();
        none[Slot(Action::Tertiary)] = Control::None;
        Check(ValidBindings(none), "an unbound action is accepted");
        ViewConfig cfg;
        Check(BindingAction(cfg, Control::East) == Action::Cancel, "east is cancel by default");
        Check(BindingAction(cfg, Control::RightThumb) == Action::ToggleCursor, "right-stick click toggles the cursor");
        Check(BindingAction(cfg, Control::LeftTrigger) == Action::None, "the left trigger has no action");
    }

    // stick_dead_zone_enter_and_exit: 0.25 to leave the centre, 0.18 to come back to it.
    {
        Ledger l;
        l.SetOwner(7);
        l.Stick(Control::LeftStick, 0.2f, 0.0f);
        Check(l.X(Control::LeftStick) == 0.0f, "0.2 from the centre is inside the dead zone");
        l.Stick(Control::LeftStick, 0.3f, 0.0f);
        Check(Near(l.X(Control::LeftStick), 0.3), "0.3 leaves the dead zone");
        l.Stick(Control::LeftStick, 0.2f, 0.0f);
        Check(Near(l.X(Control::LeftStick), 0.2), "0.2 after leaving stays out (exit dead zone 0.18)");
        l.Stick(Control::LeftStick, 0.1f, 0.0f);
        Check(l.X(Control::LeftStick) == 0.0f, "0.1 is back in the centre");
    }

    // trigger_threshold: a trigger under 0.10 is not held.
    {
        Ledger l;
        l.SetOwner(7);
        l.Button(Control::LeftTrigger, 0.05f);
        Check(!l.Held(Control::LeftTrigger), "a trigger at 0.05 is not held");
        l.Button(Control::LeftTrigger, 0.5f);
        Check(l.Held(Control::LeftTrigger), "a trigger at 0.5 is held");
    }

    // seed_blocks_held_button_until_neutral: the button that opened the page never acts in it.
    {
        Ledger l;
        PadSnapshot pad;
        pad.buttons = 512;   // RB
        l.Seed(pad);
        l.SetOwner(7);
        const Transition held = l.Button(Control::RightShoulder, 1.0f);
        Check(held.phase == Phase::None && held.consume, "RB held from before the page is consumed with no press");
        l.Button(Control::RightShoulder, 0.0f);
        Check(l.Button(Control::RightShoulder, 1.0f).phase == Phase::Press, "RB pressed again after its release is a press");
    }

    // owner_change_blocks_off_neutral: a press the game saw keeps its release.
    {
        Ledger l;
        Check(!l.Button(Control::South, 1.0f).consume, "with no owner, A goes to the game");
        l.SetOwner(7);
        const Transition up = l.Button(Control::South, 0.0f);
        Check(!up.consume && up.phase == Phase::None, "the release of a press the game saw goes to the game, not the page");
    }

    // ui_owned_release_swallowed_after_owner_leaves: a press the page owned keeps its release from the game.
    {
        Ledger l;
        l.SetOwner(7);
        Check(l.Button(Control::South, 1.0f).consume, "with an owner, A is consumed");
        l.SetOwner(0);
        Check(l.Button(Control::South, 0.0f).consume, "the release of a press the page owned is consumed after it left");
    }

    // repeat_timing: first at once, then 0.35 s, then every 0.09 s.
    {
        Ledger l;
        l.SetOwner(7);
        l.Button(Control::DpadDown, 1.0f);
        Check(l.Repeat(0.0, true, nullptr) == Control::DpadDown && l.InitialRepeat(), "the first emit is at once and initial");
        Check(l.Repeat(0.34, true, nullptr) == Control::None, "nothing before the repeat delay");
        Check(l.Repeat(0.35, true, nullptr) == Control::DpadDown && !l.InitialRepeat(), "a repeat at 0.35 s");
        Check(l.Repeat(0.43, true, nullptr) == Control::None, "nothing before the repeat interval");
        Check(l.Repeat(0.44, true, nullptr) == Control::DpadDown, "a repeat 0.09 s later");
    }

    // diagonal_hysteresis: the axis in use keeps winning near the diagonal.
    {
        Ledger l;
        l.SetOwner(7);
        l.Stick(Control::LeftStick, 0.7f, 0.55f);
        Check(l.Repeat(0.0, true, nullptr) == Control::DpadRight, "(0.7, 0.55) is right");
        l.Stick(Control::LeftStick, 0.55f, 0.6f);
        Check(l.Direction(nullptr) == Control::DpadRight, "(0.55, 0.6) stays right while right is in use");
        l.Stick(Control::LeftStick, 0.4f, 0.6f);
        Check(l.Direction(nullptr) == Control::DpadUp, "(0.4, 0.6) turns up");
    }

    // reset_device_requires_neutral: after a device reset a held stick is ignored until it centres.
    {
        Ledger l;
        l.SetOwner(7);
        l.Stick(Control::LeftStick, 0.8f, 0.0f);
        const auto gen = l.Generation();
        l.ResetDevice();
        Check(l.Generation() != gen, "a device reset is a new generation");
        l.Stick(Control::LeftStick, 0.8f, 0.0f);
        Check(l.X(Control::LeftStick) == 0.0f, "a stick held through a reset reads 0");
        l.Stick(Control::LeftStick, 0.1f, 0.0f);
        l.Stick(Control::LeftStick, 0.8f, 0.0f);
        Check(Near(l.X(Control::LeftStick), 0.8), "once centred, the stick reads again");
    }

    std::printf("%s: %d failed\n", s_failed ? "FAIL" : "PASS", s_failed);
    return s_failed;
}
