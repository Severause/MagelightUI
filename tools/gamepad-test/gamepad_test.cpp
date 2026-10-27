// The gamepad layer's rules in src/GamepadCore.h (1.6.0), on the desktop: build.bat builds and runs this; exit 0
// when every check passes.

#include "GamepadCore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

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

// A batch for view 7 holding the pad, with the pad connected.
static BatchIn Owned(double now, Style style, bool escapeCaptured = false)
{
    BatchIn in;
    in.now = now;
    in.connected = true;
    in.owner = 7;
    in.ownerEpoch = 1;
    in.config.enabled = true;
    in.config.style = style;
    in.escapeCaptured = escapeCaptured;
    return in;
}

static bool HasEvent(const BatchOut& out, Control c, Action a, Phase p)
{
    for (const Event& e : out.events)
        if (e.control == c && e.action == a && e.phase == p) return true;
    return false;
}

static bool OnlyKeys(const BatchOut& out, std::initializer_list<int> keys)
{
    return out.keys.size() == keys.size() && std::equal(keys.begin(), keys.end(), out.keys.begin());
}

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

    // navigation_press_and_repeat_events: a held d-pad direction presses at once, repeats, then releases.
    {
        Router r;
        r.Begin(Owned(0.0, Style::Navigation));
        r.Button(Control::DpadDown, 1.0f);
        const BatchOut first = r.End();
        Check(HasEvent(first, Control::DpadDown, Action::Down, Phase::Press), "d-pad down sends down/press at once");
        r.Begin(Owned(0.35, Style::Navigation));
        Check(HasEvent(r.End(), Control::DpadDown, Action::Down, Phase::Repeat), "held, it sends down/repeat at 0.35 s");
        r.Begin(Owned(0.36, Style::Navigation));
        r.Button(Control::DpadDown, 0.0f);
        Check(HasEvent(r.End(), Control::DpadDown, Action::Down, Phase::Release), "letting go sends down/release");
    }

    // keys_style_maps_to_virtual_keys: A is Enter, directions are arrows, the rest is nothing.
    {
        Router r;
        r.Begin(Owned(0.0, Style::Keys));
        r.Button(Control::South, 1.0f);
        const BatchOut a = r.End();
        Check(OnlyKeys(a, { kVkReturn }) && a.events.empty(), "keys style: A taps Enter and sends no event");
        r.Begin(Owned(0.01, Style::Keys));
        r.Button(Control::South, 0.0f);
        r.Button(Control::DpadRight, 1.0f);
        Check(OnlyKeys(r.End(), { kVkRight }), "keys style: d-pad right taps the right arrow");
        r.Begin(Owned(0.02, Style::Keys));
        r.Button(Control::DpadRight, 0.0f);
        r.Button(Control::West, 1.0f);
        const BatchOut x = r.End();
        Check(x.keys.empty() && x.events.empty(), "keys style: X sends nothing");
    }

    // B_uncaptured_consumed_and_release_swallowed: B closes through the focus menu, never reaches the page, and its
    // release never reaches the game.
    {
        Router r;
        r.Begin(Owned(0.0, Style::Navigation));
        Check(r.Button(Control::East, 1.0f), "B is consumed while a view owns the pad");
        const BatchOut b = r.End();
        Check(b.events.empty() && b.keys.empty(), "uncaptured B sends the page nothing");
        BatchIn left = Owned(0.02, Style::Navigation);
        left.owner = 0;
        r.Begin(left);
        Check(r.Button(Control::East, 0.0f), "B's release after the view closed is consumed");
        Router k;
        k.Begin(Owned(0.0, Style::Keys));
        k.Button(Control::East, 1.0f);
        Check(k.End().keys.empty(), "keys style: uncaptured B taps no Escape");
    }

    // B_captured_reaches_page: a page that took Escape gets B as cancel (or Escape).
    {
        Router r;
        r.Begin(Owned(0.0, Style::Navigation, true));
        r.Button(Control::East, 1.0f);
        Check(HasEvent(r.End(), Control::East, Action::Cancel, Phase::Press), "captured B sends cancel/press");
        Router k;
        k.Begin(Owned(0.0, Style::Keys, true));
        k.Button(Control::East, 1.0f);
        Check(OnlyKeys(k.End(), { kVkEscape }), "keys style: captured B taps Escape");
    }

    // owner_gain_with_held_shoulder_sends_no_tab: the shoulder that cast the opening power is not a tab change.
    {
        Router r;
        BatchIn in = Owned(0.0, Style::Navigation);
        in.pad.buttons = 512;   // RB
        r.Begin(in);
        r.Button(Control::RightShoulder, 1.0f);
        const BatchOut held = r.End();
        Check(held.events.empty(), "RB held through the opening sends nothing");
        r.Begin(Owned(0.02, Style::Navigation));
        r.Button(Control::RightShoulder, 0.0f);
        r.Button(Control::RightShoulder, 1.0f);
        Check(HasEvent(r.End(), Control::RightShoulder, Action::NextTab, Phase::Press), "RB pressed again is nextTab");
    }

    // disconnect_stops_repeat_and_reconnect_needs_neutral.
    {
        Router r;
        r.Begin(Owned(0.0, Style::Navigation));
        r.Button(Control::DpadUp, 1.0f);
        r.End();
        BatchIn off = Owned(0.40, Style::Navigation);
        off.connected = false;
        r.Begin(off);
        const BatchOut gone = r.End();
        Check(gone.events.empty(), "a disconnected pad repeats nothing");
        BatchIn back = Owned(0.50, Style::Navigation);
        back.pad.buttons = 1;   // d-pad up still held
        r.Begin(back);
        r.Button(Control::DpadUp, 1.0f);
        const BatchOut again = r.End();
        Check(again.events.empty(), "reconnecting with up held sends no press");
    }

    // no_owner_passes_through.
    {
        Router r;
        BatchIn in = Owned(0.0, Style::Navigation);
        in.owner = 0;
        r.Begin(in);
        Check(!r.Button(Control::South, 1.0f), "with no owner, A is not consumed");
        Check(r.End().events.empty(), "with no owner, nothing is sent");
    }

    // reload_epoch_rearms: a reload of the owning page is a new owner for the held-button rules.
    {
        Router r;
        r.Begin(Owned(0.0, Style::Navigation));
        r.Button(Control::South, 1.0f);
        r.End();
        BatchIn reloaded = Owned(0.02, Style::Navigation);
        reloaded.ownerEpoch = 2;
        reloaded.pad.buttons = 4096;   // A still held
        r.Begin(reloaded);
        r.Button(Control::South, 1.0f);
        const BatchOut out = r.End();
        Check(out.reset && !HasEvent(out, Control::South, Action::Accept, Phase::Press), "after a reload, held A is no press and the batch resets");
    }

    // cursor_mode_moves_and_clicks.
    {
        Router r;
        r.tuning = Tuning{};
        BatchIn in = Owned(0.0, Style::Navigation);
        in.config.allowCursor = true;
        r.Begin(in);
        r.Button(Control::RightThumb, 1.0f);
        const BatchOut toggled = r.End();
        Check(toggled.mode == Mode::Cursor, "right-stick click enters cursor mode");
        in.now = 0.05;
        in.mode = toggled.mode;
        r.Begin(in);
        r.Button(Control::RightThumb, 0.0f);
        r.Stick(Control::LeftStick, 1.0f, 0.0f);
        const BatchOut moved = r.End();
        Check(Near(moved.pointer.dx, 45.0) && moved.pointer.dy == 0.0f, "full right for 0.05 s moves 45 px (900 x 0.05)");
        in.now = 0.06;
        r.Begin(in);
        r.Stick(Control::LeftStick, 0.0f, 0.0f);
        r.Button(Control::South, 1.0f);
        r.Button(Control::South, 0.0f);
        const BatchOut clicked = r.End();
        Check(clicked.pointer.buttons.size() == 2 && clicked.pointer.buttons[0] && !clicked.pointer.buttons[1],
              "A in cursor mode is one left down and up");
        Check(!HasEvent(clicked, Control::South, Action::Accept, Phase::Press), "A in cursor mode does not also accept");
        in.now = 0.07;
        r.Begin(in);
        r.Button(Control::DpadDown, 1.0f);
        const BatchOut back = r.End();
        Check(back.mode == Mode::Navigation && back.pointer.release, "a d-pad press returns to navigation mode and releases");
    }

    // mouse_activity_releases_cursor_press.
    {
        Router r;
        BatchIn in = Owned(0.0, Style::Navigation);
        in.config.allowCursor = true;
        in.mode = Mode::Cursor;
        r.Begin(in);
        r.Button(Control::South, 1.0f);
        r.OtherDeviceActivity();
        const BatchOut out = r.End();
        Check(out.pointer.release && !out.gamepadDevice, "a mouse move during a cursor press releases it and hands the device back");
    }

    // overflow_resets: more than 128 events in a batch are dropped and the pad must return to neutral.
    {
        Router r;
        r.Begin(Owned(0.0, Style::Navigation));
        for (int i = 0; i < 129; ++i) r.Button(Control::South, (i % 2) ? 0.0f : 1.0f);
        const BatchOut out = r.End();
        Check(out.events.empty() && out.reset, "129 events in one batch are dropped with a reset");
    }

    // packet_shape: what the page core's helper parses.
    {
        ViewConfig cfg;
        cfg.enabled = true;
        cfg.style = Style::Navigation;
        std::vector<Event> events{ Event{ Control::DpadDown, Action::Down, Phase::Press, 1.0f, 0.0f, 0.0f } };
        PacketIn p;
        p.page = "p-1";
        p.generation = 3;
        p.sequence = 9;
        p.active = true;
        p.enabled = true;
        p.connected = true;
        p.style = Style::Navigation;
        p.gamepadDevice = true;
        p.config = &cfg;
        p.dt = 0.2;
        p.events = &events;
        const std::string json = PacketJson(p);
        const auto has = [&](const char* s) { return json.find(s) != std::string::npos; };
        Check(has("\"version\":1") && has("\"page\":\"p-1\"") && has("\"generation\":3") && has("\"sequence\":9"),
              "the packet names its version, page, generation and sequence");
        Check(has("\"style\":\"navigation\"") && has("\"mode\":\"navigation\"") && has("\"device\":\"gamepad\"") &&
              has("\"glyphFamily\":\"xbox\""), "the packet carries style, mode, device and glyphs");
        Check(has("\"events\":[{\"control\":\"dpadDown\",\"action\":\"down\",\"phase\":\"press\""), "the event is spelled out");
        Check(has("\"bindings\":{\"up\":\"dpadUp\"") && has("\"toggleCursor\":\"rightThumb\""), "the bindings are spelled out");
        Check(has("\"dt\":0.05"), "dt is capped at 0.05");
        Check(ValidPageId("p-1") && !ValidPageId("a b") && !ValidPageId(std::string(81, 'a')) && !ValidPageId(""),
              "page ids are 1 to 80 letters, digits, '-' or '_'");
    }

    std::printf("%s: %d failed\n", s_failed ? "FAIL" : "PASS", s_failed);
    return s_failed;
}
