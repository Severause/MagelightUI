// Magelight UI — Skyrim VR presenter: controller input from the game's input sink
// and the held-button masks the UI-mode exit reads (docs/VR_PRESENTER.md §6).
// Shared state and its rules: State.h.

#include "State.h"

namespace Magelight::VR {

    // ── Input edges (game thread writes, present thread reads) ──────────────
    HandInput s_hands[2];   // 0 = left, 1 = right (physical), resolved via IsLeftHandedMode at use
    // HeldButtonsNow: SubmitFrame writes the runtime half, NoteButton the engine half; each keeps the
    // GetTickCount64 of every bit's last press for HeldButtonsPressedSince.
    std::atomic<std::uint32_t> s_runtimeHeldMask{ 0 };

    namespace {
        std::atomic<std::uint32_t> s_engineHeldMask{ 0 };
    }

    std::atomic<std::uint64_t> s_runtimePressMs[12]{};

    namespace {
        std::atomic<std::uint64_t> s_enginePressMs[12]{};

        int HandIndexForDevice(int device)
        {
            using D = RE::INPUT_DEVICE;
            switch (static_cast<D>(device)) {
            case D::kVivePrimary: case D::kOculusPrimary: case D::kWMRPrimary: return 1;     // primary = right (physical)
            case D::kViveSecondary: case D::kOculusSecondary: case D::kWMRSecondary: return 0;
            default: return -1;
            }
        }
    }

    void NoteButton(int device, std::uint32_t code, float value)
    {
        const int h = HandIndexForDevice(device);
        if (h < 0) return;
        using K = RE::BSOpenVRControllerDevice::Keys;
        HandInput& hi = s_hands[h];
        constexpr float kPressLevel = 0.5f;   // analog axes: half travel = pressed
        const bool down = value > kPressLevel;
        // HeldButtonsNow's engine half: kHeldButtons index 0 grip, 1 trigger, 2 B/Y.
        const int held = code == K::kTrigger ? 1 : (code == K::kGrip || code == K::kGripAlt) ? 0 : code == K::kBY ? 2 : -1;
        if (held >= 0) {
            const int index = h * 6 + held;
            const std::uint32_t bit = 1u << index;
            if (down) {
                if (!(s_engineHeldMask.fetch_or(bit) & bit)) s_enginePressMs[index].store(GetTickCount64());
            } else {
                s_engineHeldMask.fetch_and(~bit);
            }
        }
        if (code == K::kTrigger) {
            hi.triggerDown.store(down);        // LEVEL; TickLaser derives the click edges
        } else if (code == K::kGrip || code == K::kGripAlt) {
            if (down && !hi.gripWasDown) hi.gripEdge.store(true);   // rising edge only
            hi.gripWasDown = down;
        } else if (code == K::kBY) {
            if (down && !hi.menuWasDown) hi.menuEdge.store(true);
            hi.menuWasDown = down;
        }
    }

    void NoteThumbstick(int device, float /*x*/, float y)
    {
        const int h = HandIndexForDevice(device);
        if (h < 0) return;
        s_hands[h].stickY_mil.store(static_cast<int>(std::lround(std::clamp(y, -1.0f, 1.0f) * 1000.0f)));
        s_hands[h].stickTick.store(GetTickCount64());
    }

    // Present thread only, like every other OpenVR call here.
    std::uint32_t PollHeldMask()
    {
        if (!s_system) return 0;
        std::uint32_t mask = 0;
        for (int hand = 0; hand < 2; ++hand) {
            const auto role = (hand == 1) ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand;
            const vr::TrackedDeviceIndex_t idx = s_system->GetTrackedDeviceIndexForControllerRole(role);
            if (idx == vr::k_unTrackedDeviceIndexInvalid || idx >= vr::k_unMaxTrackedDeviceCount) continue;
            vr::VRControllerState_t st{};
            if (!s_system->GetControllerState(idx, &st, sizeof(st))) continue;
            for (std::size_t i = 0; i < std::size(kHeldButtons); ++i)
                if (ButtonHeld(st, kHeldButtons[i])) mask |= 1u << (hand * 6 + i);
        }
        return mask;
    }

    HeldButtons HeldButtonsNow() { return { s_runtimeHeldMask.load(), s_engineHeldMask.load() }; }

    HeldButtons HeldButtonsPressedSince(std::uint64_t sinceTickMs)
    {
        auto since = [&](std::uint32_t mask, const std::atomic<std::uint64_t>* pressMs) {
            std::uint32_t out = 0;
            for (int i = 0; i < 12; ++i)
                if ((mask & (1u << i)) && pressMs[i].load() >= sinceTickMs) out |= 1u << i;
            return out;
        };
        return { since(s_runtimeHeldMask.load(), s_runtimePressMs), since(s_engineHeldMask.load(), s_enginePressMs) };
    }

}  // namespace Magelight::VR
