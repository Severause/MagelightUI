// Magelight UI — Skyrim VR presenter: controller hotkey bindings and the
// press-to-bind listener (MagelightVR.h, VR controller hotkeys; the VR-4 entry
// of docs/VR_PRESENTER.md §10a). Shared state and its rules: State.h.

#include "State.h"

#include "MagelightApi4.h"             // DispatchViewAction (game-thread gated action)
#include "../../api/MagelightUI_API.h" // kHotkeyAction*

namespace Magelight::VR {

    namespace {
        // ── VR controller hotkey bindings (VR-4) ──────────────────────
        struct VRBinding {
            std::uint32_t button = 0, modifier = 0, action = 0;
            std::uint8_t  hand = 0;
            HotkeyFn      cb = nullptr;   // set = call the mod instead of running `action`
            void*         user = nullptr;
        };
        std::mutex s_bindMutex;
        std::map<ViewId, VRBinding> s_bindings;
        // Global bind epoch: bumped under s_bindMutex on ANY (re)bind, unbind,
        // or capture arm/disarm. The dispatch loop re-latches EVERY view when
        // the epoch it last saw changed, so a chord still physically held when
        // capture ends — or when another view is rebound — can never fire a
        // bystander view's action.
        std::uint32_t               s_bindEpoch = 0;   // guarded by s_bindMutex
        // Present-thread only: seed the capture masks on the first tick after
        // arming. The 0xFFFFFFFF "suppress everything currently held" seed is
        // written by the OWNER of these arrays (the tick), never by
        // SetButtonListener, which runs on its caller's thread — that
        // cross-thread write would race the tick's own writes.
        bool                        s_edgeSeedPending = false;  // guarded by s_bindMutex
        std::map<ViewId, bool>      s_bindWas;       // present thread: rising-edge latch per view
        std::map<ViewId, std::uint32_t> s_bindWasEpoch;  // present thread: bind epoch the latch last reset for
        std::map<ViewId, std::uint64_t> s_bindLastTick;  // present thread: PER-VIEW debounce
        std::uint64_t               s_captureLastTick = 0;  // present thread: the capture branch's own debounce

        // Press-to-bind listener (guarded by s_bindMutex). While set, the
        // hotkey tick reports raw button edges here INSTEAD of dispatching
        // bindings — the press being captured must not also open a menu.
        ButtonEdgeFn  s_edgeCb   = nullptr;
        void*         s_edgeUser = nullptr;
        std::uint32_t s_edgeWasMask[2] = { 0, 0 };   // per hand: known buttons held last frame
        // A modifier-class button (Grip, Trigger) pressed ALONE is not
        // reported at once: it may be the HOLD half of a chord. It sits here
        // until a second button on the same hand fires (→ reported as that
        // button with this one held) or it is released with nothing else
        // pressed (→ reported bare), so pressing Grip to start "Grip + A"
        // does not capture a bare Grip.
        std::uint32_t s_edgePending[2] = { 0, 0 };
        // The chord vocabulary, in the order a chord's MODIFIER is reported
        // when more than one other button is held (Grip and Trigger are what
        // people naturally hold while pressing a face button).
        constexpr std::uint32_t kEdgeCodes[] = { 2, 33, 1, 7, 32, 35 };
    }

    bool ButtonHeld(const vr::VRControllerState_t& st, std::uint32_t button)
    {
        if (button == 0) return true;   // no modifier required
        if ((st.ulButtonPressed & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(button))) != 0) return true;
        // Analog fallbacks: trigger and grip report through their axes on
        // Touch/Index even when the digital bit never sets.
        if (button == 33 && st.rAxis[1].x > 0.5f) return true;
        if (button == 2  && st.rAxis[2].x > 0.5f) return true;
        return false;
    }

    // ── VR hotkeys: polled every frame, UI mode or not ────────────
    void TickVRHotkeys()
    {
        if (!s_system) return;
        std::map<ViewId, VRBinding> binds;
        ButtonEdgeFn edgeCb = nullptr;
        void*        edgeUser = nullptr;
        std::uint32_t epoch = 0;
        bool          doSeed = false;
        {
            std::lock_guard<std::mutex> lk(s_bindMutex);
            edgeCb = s_edgeCb; edgeUser = s_edgeUser;
            epoch  = s_bindEpoch;
            if (edgeCb) { doSeed = s_edgeSeedPending; s_edgeSeedPending = false; }
            if (s_bindings.empty() && !edgeCb) return;
            binds = s_bindings;
        }
        vr::VRControllerState_t st[2]{};
        bool have[2] = { false, false };
        for (int hand = 0; hand < 2; ++hand) {
            const auto role = (hand == 1) ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand;
            const vr::TrackedDeviceIndex_t idx = s_system->GetTrackedDeviceIndexForControllerRole(role);
            if (idx == vr::k_unTrackedDeviceIndexInvalid || idx >= vr::k_unMaxTrackedDeviceCount) continue;
            have[hand] = s_system->GetControllerState(idx, &st[hand], sizeof(st[hand]));
        }
        if (!have[0] && !have[1]) return;

        // ── Capture mode: report edges, dispatch nothing ──────────────
        if (edgeCb) {
            if (doSeed) {   // first tick after arm — seed on the owning thread
                s_edgeWasMask[0] = s_edgeWasMask[1] = 0xFFFFFFFFu;
                s_edgePending[0] = s_edgePending[1] = 0;
            }
            auto isModifierClass = [](std::uint32_t code) { return code == 2 || code == 33; };
            for (int hand = 0; hand < 2; ++hand) {
                if (!have[hand]) { s_edgeWasMask[hand] = 0; s_edgePending[hand] = 0; continue; }
                std::uint32_t mask = 0;
                for (std::size_t i = 0; i < std::size(kEdgeCodes); ++i)
                    if (ButtonHeld(st[hand], kEdgeCodes[i])) mask |= (1u << i);
                const std::uint32_t fresh = mask & ~s_edgeWasMask[hand];
                s_edgeWasMask[hand] = mask;

                std::uint32_t button = 0, heldOther = 0;
                if (fresh) {
                    // The pressed button is the newly-down one; the modifier
                    // is the first OTHER known button already held, in
                    // kEdgeCodes preference order.
                    for (std::size_t i = 0; i < std::size(kEdgeCodes); ++i)
                        if (fresh & (1u << i)) { button = kEdgeCodes[i]; break; }
                    for (std::size_t i = 0; i < std::size(kEdgeCodes); ++i)
                        if ((mask & (1u << i)) && kEdgeCodes[i] != button) { heldOther = kEdgeCodes[i]; break; }
                    if (isModifierClass(button) && heldOther == 0) {
                        // Alone: maybe the HOLD half. Wait for a second
                        // button or the release.
                        s_edgePending[hand] = button;
                        continue;
                    }
                } else if (s_edgePending[hand]) {
                    // No new press this tick: the pending modifier either
                    // is still held (keep waiting) or was released bare.
                    bool stillHeld = false;
                    for (std::size_t i = 0; i < std::size(kEdgeCodes); ++i)
                        if (kEdgeCodes[i] == s_edgePending[hand]) stillHeld = (mask & (1u << i)) != 0;
                    if (stillHeld) continue;
                    button = s_edgePending[hand];
                    heldOther = 0;
                } else {
                    continue;
                }
                s_edgePending[hand] = 0;
                const std::uint64_t now = GetTickCount64();
                if (now - s_captureLastTick < 250) continue;
                s_captureLastTick = now;
                SKSE::log::info("Magelight VR: button edge captured — button {} held {} hand {}",
                                button, heldOther, hand);
                edgeCb(button, heldOther, static_cast<std::uint8_t>(hand), edgeUser);
                break;   // one edge per tick is plenty for a capture UI
            }
            return;
        }

        for (const auto& [view, b] : binds) {
            if (!b.button) continue;
            bool down = false;
            for (int hand = 0; hand < 2; ++hand) {
                if (!have[hand]) continue;
                if (b.hand == 1 && hand != 0) continue;
                if (b.hand == 2 && hand != 1) continue;
                if (ButtonHeld(st[hand], b.button) && ButtonHeld(st[hand], b.modifier)) { down = true; break; }
            }
            bool& was = s_bindWas[view];
            auto& seenEpoch = s_bindWasEpoch[view];
            if (seenEpoch != epoch) {
                // The bind set changed (a (re)bind, an unbind, or capture
                // arming/ending). Re-latch EVERY view to live state so a
                // chord still physically held — from the press that chose
                // it, or from an unrelated view's chord — must be released
                // before it can fire. Reseeding only the rebound view would
                // let bystander views fire on a held-through chord.
                seenEpoch = epoch;
                was = down;
                continue;
            }
            if (down && !was) {
                // Per-view debounce: a single global timestamp would let one
                // view's fire swallow another view's distinct hotkey within
                // 250ms.
                std::uint64_t& lastTick = s_bindLastTick[view];
                const std::uint64_t now = GetTickCount64();
                if (now - lastTick >= 250) {
                    lastTick = now;
                    // A mod that owns its own open/close semantics gets the
                    // edge raw; everyone else gets the built-in action.
                    if (b.cb) b.cb(view, b.user);
                    else      Api4::DispatchViewAction(view, b.action, "vr");
                }
            }
            was = down;
        }
    }

    void BindHotkey(ViewId view, std::uint32_t button, std::uint32_t modifier,
                    std::uint8_t hand, std::uint32_t action)
    {
        std::lock_guard<std::mutex> lk(s_bindMutex);
        if (!button) { s_bindings.erase(view); ++s_bindEpoch; return; }
        s_bindings[view] = VRBinding{ button, modifier, action, hand, nullptr, nullptr };
        ++s_bindEpoch;
        SKSE::log::info("Magelight VR: view {} bound to controller button {} (modifier {}, hand {}, action {})",
            view, button, modifier, static_cast<int>(hand), action);
    }

    void SetButtonListener(ButtonEdgeFn cb, void* user)
    {
        std::lock_guard<std::mutex> lk(s_bindMutex);
        s_edgeCb = cb; s_edgeUser = user;
        // Every bound view must re-latch on the next tick — a chord held
        // through the capture must not fire a view when capture ends.
        ++s_bindEpoch;
        // Ask the present-thread tick to seed the capture masks ("suppress
        // everything currently held, so the button the user clicked Rebind
        // with is not read as a fresh edge"). Writing that seed HERE, on the
        // caller's thread, would race the tick's own mask writes.
        s_edgeSeedPending = (cb != nullptr);
        SKSE::log::info("Magelight VR: button listener {}", cb ? "armed — bindings suppressed" : "cleared");
    }

    void BindHotkeyCallback(ViewId view, std::uint32_t button, std::uint32_t modifier,
                            std::uint8_t hand, HotkeyFn cb, void* user)
    {
        std::lock_guard<std::mutex> lk(s_bindMutex);
        if (!button || !cb) { s_bindings.erase(view); ++s_bindEpoch; return; }
        s_bindings[view] = VRBinding{ button, modifier, 0u, hand, cb, user };
        ++s_bindEpoch;
        SKSE::log::info("Magelight VR: view {} bound to controller button {} (modifier {}, hand {}) -> mod callback",
            view, button, modifier, static_cast<int>(hand));
    }

    bool ViewBindsButton(ViewId view, std::uint32_t openvrButton)
    {
        if (!view || !openvrButton) return false;
        std::lock_guard<std::mutex> lk(s_bindMutex);
        const auto it = s_bindings.find(view);
        return it != s_bindings.end() && it->second.button == openvrButton;
    }

    void UnbindView(ViewId view)
    {
        {
            std::lock_guard<std::mutex> lk(s_bindMutex);
            s_bindings.erase(view);
            ++s_bindEpoch;
        }
        s_bindWas.erase(view);
        s_bindWasEpoch.erase(view);
        s_bindLastTick.erase(view);
        if (s_kbView == view) { s_kbView = 0; }
    }

}  // namespace Magelight::VR
