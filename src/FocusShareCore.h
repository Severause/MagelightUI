#pragma once
// Focus sharing (1.9.0, SetViewFocusSharing; CLAUDE.md invariant 30): while one view holds UI mode, the keyboard goes
// to the key view, which is the holder's unless a click moved it to a view that takes it (any mod's). UI mode itself
// (the cursor, the pause, Escape's close, the gamepad, who owns it) stays the holder's: only keys, typed text, the IME
// and the page's focus move.
//
// std only: tools/focusshare-test runs all of it on the desktop.

#include <cstdint>

namespace Magelight::FocusShare {

    // SetViewFocusSharing's flags (MAGELIGHT_API::kFocusShareGive / kFocusShareTake).
    inline constexpr std::uint32_t kGive = 1;   // while this view holds UI mode, a click on a view that takes moves the keyboard
    inline constexpr std::uint32_t kTake = 2;   // a click on this view takes the keyboard from a holder that gives
    inline constexpr std::uint32_t kKnown = kGive | kTake;

    // What the rules read of one view, under the registry lock.
    struct Facts {
        bool exists = false;        // in the registry, not being destroyed, with a page
        bool interactive = false;   // shown here and not click-through: a press can land on it
        std::uint32_t flags = 0;    // kGive / kTake
    };

    // The view a left press inside `pressed` moves the keyboard to, or 0 to leave it where it is. holder: the UI-mode
    // view (0 outside UI mode); keyView: where the keyboard is now. A press on the holder always takes it back; a press
    // on another view moves it only when the holder gives and that view takes.
    inline std::uint64_t OnPress(std::uint64_t holder, std::uint64_t keyView, std::uint64_t pressed, const Facts& holderFacts,
                                 const Facts& pressedFacts)
    {
        if (!holder || !pressed || pressed == keyView) return 0;
        if (pressed == holder) return holder;
        if (!(holderFacts.flags & kGive) || !(pressedFacts.flags & kTake)) return 0;
        if (!pressedFacts.exists || !pressedFacts.interactive) return 0;
        return pressed;
    }

    // Where the keyboard belongs now: keyView while the share still holds, else the holder (0 outside UI mode). It
    // goes back when the holder stops giving, or the view that has it stops taking, goes away or stops showing.
    inline std::uint64_t Settle(std::uint64_t holder, std::uint64_t keyView, const Facts& holderFacts, const Facts& keyFacts)
    {
        if (!holder || !keyView || keyView == holder) return holder;
        const bool holds = (holderFacts.flags & kGive) && (keyFacts.flags & kTake) && keyFacts.exists && keyFacts.interactive;
        return holds ? keyView : holder;
    }

    // Whether flags names only known bits.
    inline bool KnownFlags(std::uint32_t flags) { return (flags & ~kKnown) == 0; }

}  // namespace Magelight::FocusShare
