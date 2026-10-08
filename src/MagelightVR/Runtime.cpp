// Magelight UI — Skyrim VR presenter: the OpenVR runtime binding, the presenter
// state and settings, and the per-frame pose snapshot (docs/VR_PRESENTER.md).
// OpenVR comes from the openvr_api.dll the game loaded, through GetProcAddress:
// nothing links an OpenVR import library. Shared state and its rules: State.h.

#include "State.h"

namespace Magelight::VR {

    // ── Runtime binding ─────────────────────────────────────────────────────
    namespace {
        using GetGenericInterfaceFn = void* (*)(const char*, vr::EVRInitError*);
        using IsInterfaceVersionValidFn = bool (*)(const char*);

        std::atomic<int>  s_state{ static_cast<int>(State::Dormant) };
    }

    bool              s_isVR = false;          // REL::Module::IsVR(), latched in Init
    Settings          s_settings;              // Magelight.json "vr"

    vr::IVRSystem*     s_system = nullptr;
    vr::IVRCompositor* s_compositor = nullptr;
    vr::IVROverlay*    s_overlay = nullptr;    // the CLEAN interface (texture submission works)
    vr::IVRRenderModels* s_renderModels = nullptr;   // optional: the controller "tip" aim frame
    std::uint32_t      s_initToken = 0;

    namespace {
        GetGenericInterfaceFn     s_getInterface = nullptr;
        IsInterfaceVersionValidFn s_isVersionValid = nullptr;
    }

    GetInitTokenFn            s_getInitToken = nullptr;

    ID3D11Device*        s_device = nullptr;   // not owned (the host's refs)
    ID3D11DeviceContext* s_context = nullptr;

    void SetState(State s) { s_state.store(static_cast<int>(s)); }

    // Resolve the three interfaces against the EXACT version strings our
    // header's vtables describe (design §7: never ask for a newer one).
    bool BindInterfaces()
    {
        const HMODULE mod = GetModuleHandleA("openvr_api.dll");
        if (!mod) {
            SKSE::log::warn("Magelight VR: openvr_api.dll is not loaded in this process — presenter dormant");
            return false;
        }
        s_getInterface   = reinterpret_cast<GetGenericInterfaceFn>(GetProcAddress(mod, "VR_GetGenericInterface"));
        s_isVersionValid = reinterpret_cast<IsInterfaceVersionValidFn>(GetProcAddress(mod, "VR_IsInterfaceVersionValid"));
        s_getInitToken   = reinterpret_cast<GetInitTokenFn>(GetProcAddress(mod, "VR_GetInitToken"));
        if (!s_getInterface) {
            SKSE::log::error("Magelight VR: VR_GetGenericInterface not exported by openvr_api.dll — presenter dormant");
            return false;
        }
        auto bind = [&](const char* version, void*& out) -> bool {
            if (s_isVersionValid && !s_isVersionValid(version)) {
                SKSE::log::error("Magelight VR: runtime does not serve {} — presenter dormant", version);
                return false;
            }
            vr::EVRInitError err = vr::VRInitError_None;
            out = s_getInterface(version, &err);
            if (!out || err != vr::VRInitError_None) {
                SKSE::log::error("Magelight VR: {} unavailable (init error {}) — presenter dormant",
                    version, static_cast<int>(err));
                return false;
            }
            return true;
        };
        void* sys = nullptr; void* comp = nullptr; void* ovl = nullptr;
        if (!bind(vr::IVRSystem_Version, sys)) return false;
        if (!bind(vr::IVRCompositor_Version, comp)) return false;
        if (!bind(vr::IVROverlay_Version, ovl)) return false;
        s_system = static_cast<vr::IVRSystem*>(sys);
        s_compositor = static_cast<vr::IVRCompositor*>(comp);
        s_overlay = static_cast<vr::IVROverlay*>(ovl);
        // Render models are OPTIONAL — only the aim transform uses them, and
        // the fixed-pitch fallback covers a runtime that does not serve them.
        //
        // OpenComposite (OpenVR re-implemented over OpenXR; OCU on Skyrim VR)
        // SERVES IVRRenderModels but stubs RenderModelHasComponent: the
        // call pops its "Hit stubbed file" dialog every time. It is
        // recognisable by the factory exports stock openvr_api.dll never
        // has (HmdSystemFactory, VRSystem, ...), so don't touch render
        // models there at all.
        const bool openComposite = GetProcAddress(mod, "HmdSystemFactory") != nullptr;
        if (openComposite) {
            SKSE::log::info("Magelight VR: OpenComposite detected (HmdSystemFactory export) — "
                            "render-model tip aim skipped (stubbed there); aim uses the fixed pitch");
        } else {
            vr::EVRInitError rmErr = vr::VRInitError_None;
            s_renderModels = static_cast<vr::IVRRenderModels*>(
                s_getInterface(vr::IVRRenderModels_Version, &rmErr));
            if (!s_renderModels)
                SKSE::log::info("Magelight VR: {} unavailable ({}) — aim falls back to the fixed pitch",
                    vr::IVRRenderModels_Version, static_cast<int>(rmErr));
        }
        s_initToken = s_getInitToken ? s_getInitToken() : 0;
        SKSE::log::info("Magelight VR: bound {} / {} / {} (init token {}); tracking space {}",
            vr::IVRSystem_Version, vr::IVRCompositor_Version, vr::IVROverlay_Version, s_initToken,
            static_cast<int>(s_compositor->GetTrackingSpace()));
        return true;
    }

    // ── Per-frame pose snapshot (present thread) ────────────────────────────
    vr::TrackedDevicePose_t s_frameRender[vr::k_unMaxTrackedDeviceCount];
    bool  s_frameHmdUsable = false;
    float s_frameDt = 0.0f;

    bool PoseUsable(const vr::TrackedDevicePose_t& p)
    {
        // Running_OutOfRange is a NORMAL, recoverable state (you stepped
        // outside the play space, a base station lost you for a moment).
        // The pose it carries is still valid — bPoseIsValid says so — and
        // demanding Running_OK would leave a panel unplaced and the laser
        // dead for the duration. Everything else (Uninitialised,
        // calibrating, fallback-rotation-only) stays out.
        if (!p.bPoseIsValid || !p.bDeviceIsConnected) return false;
        return p.eTrackingResult == vr::TrackingResult_Running_OK ||
               p.eTrackingResult == vr::TrackingResult_Running_OutOfRange;
    }

    // ── Settings and state queries ──────────────────────────────────────────
    // Configure runs once, at settings load before Init; the queries may be
    // called from any thread.
    void Configure(const Settings& s) { s_settings = s; }
    bool MirrorEnabled() { return s_settings.mirror || GetState() != State::Live; }
    bool SuppressRuntimeLaser() { return s_settings.suppressRuntimeLaser && GetState() == State::Live; }
    bool OwnKeyboardEnabled() { return s_settings.ownKeyboard && GetState() == State::Live; }
    float CursorScale() { return std::clamp(s_settings.cursorScale, 0.004f, 0.5f); }
    bool  CursorDot()   { return s_settings.cursorDot; }

    Mode DefaultPanelMode()
    {
        return s_settings.panelFollow ? Mode::LazyFollow : Mode::WorldLocked;
    }

    bool PanelSizeForFullscreen(int& w, int& h)
    {
        if (GetState() != State::Live || !s_settings.submitViews) return false;
        if (s_settings.panelWidth <= 0 || s_settings.panelHeight <= 0) return false;
        w = s_settings.panelWidth;
        h = s_settings.panelHeight;
        return true;
    }

    State GetState() { return static_cast<State>(s_state.load()); }
    bool  IsVRRuntime() { return s_isVR; }

    const char* StateName(State s)
    {
        switch (s) {
        case State::Dormant: return "dormant";
        case State::Pending: return "pending";
        case State::Live:    return "live";
        case State::Failed:  return "failed";
        }
        return "?";
    }

}  // namespace Magelight::VR
