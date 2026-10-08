#pragma once
// Magelight UI — Skyrim VR presenter, the state its translation units share
// (src/MagelightVR/*.cpp; design: docs/VR_PRESENTER.md). Each variable and
// function declared here is defined in exactly one of those files; the types
// and ClampWidthM are defined here. Anything one file uses alone stays in that
// file's anonymous namespace. A `static` redefinition of a name declared here
// is a compile error (/we4211); without the flag MSVC gives that file its own copy.
//
// Rules a change must keep:
//  - Threads (MagelightVR.h): OpenVR is called on the present (= Ultralight)
//    thread only, from Init, SubmitFrame, ReleaseView, Shutdown and what they
//    call. Button levels arrive on the game thread through NoteButton /
//    NoteThumbstick; what they share with the present thread is atomic, no
//    lock. Placement requests may come from any thread and go through
//    s_placementMutex; the binding table is behind s_bindMutex (Bindings.cpp).
//  - The pose snapshot (s_frameRender, s_frameHmdUsable, s_frameDt) is taken
//    once per frame in SubmitFrame and shared by the follow glide and the
//    laser, on the present thread.
//  - Overlay records: the present thread mutates s_overlays lock-free; other
//    threads read placements through s_placementSnapshot, never s_overlays.
//    Records are inserted and erased only in Lifecycle.cpp (SubmitFrame,
//    ReleaseView, Shutdown); Overlays.cpp holds the per-record operations;
//    Laser.cpp writes fields of the records it finds (the head-locked world
//    matrix, grip recentre, grab-and-move).
//  - The beam overlays are session-global like the probe: released only by
//    ReleaseBeams (Shutdown), never in ReleaseView; ForgetBeams drops the
//    handles of a restarted runtime.

#include "MagelightVR.h"

// CommonLib first: REX/W32 refuses a translation unit that saw the Windows
// API before it did.
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include <d3d11.h>
#include <windows.h>

// Only the presenter's translation units see OpenVR (design §8).
#include <openvr.h>

namespace Magelight::VR {

    // ── Runtime.cpp: binding, state, settings, the pose snapshot ───────────────
    using GetInitTokenFn = std::uint32_t (*)();

    extern bool s_isVR;
    extern Settings s_settings;
    extern DWORD s_thread;
    extern vr::VROverlayHandle_t s_probe;
    extern vr::IVRSystem* s_system;
    extern vr::IVRCompositor* s_compositor;
    extern vr::IVROverlay* s_overlay;
    extern vr::IVRRenderModels* s_renderModels;
    extern std::uint32_t s_initToken;
    extern GetInitTokenFn s_getInitToken;
    extern ID3D11Device* s_device;
    extern ID3D11DeviceContext* s_context;
    extern const char* kOverlayKeyPrefix;
    void SetState(State s);
    bool BindInterfaces();
    extern vr::TrackedDevicePose_t s_frameRender[vr::k_unMaxTrackedDeviceCount];
    extern bool s_frameHmdUsable;
    extern float s_frameDt;
    extern std::chrono::steady_clock::time_point s_lastFrameTp;
    bool PoseUsable(const vr::TrackedDevicePose_t& p);
    void CreateProbeOverlay();

    // ── Overlays.cpp: overlay records and placement ─────────────────────────────
    struct OverlayRec {
        vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
        ID3D11Texture2D*          tex = nullptr;   // host-owned copy target (design §2)
        ID3D11RenderTargetView*   rtv = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        int texW = 0, texH = 0;
        // Last frame's stamped cursor marks, so a still page whose laser
        // dot moved still gets redrawn. -1 = none last frame.
        float lastMarkU[2] = { -1.0f, -1.0f }, lastMarkV[2] = { -1.0f, -1.0f };
        int   lastMarkCount = -1;
        bool  everSubmitted = false;
        bool shown = false;
        bool placed = false;                        // transform issued at least once
        Placement placement;
        vr::HmdMatrix34_t world{};                  // last authored pose (compositor space) — the laser hits THIS
        // LazyFollow / WorldLocked state: level panel = horizontal forward + position.
        float fwd[2] = { 0.0f, -1.0f };             // XZ unit forward the panel faces AWAY from the head along
        float pos[3] = { 0.0f, 0.0f, 0.0f };
        bool  following = false;                    // mid catch-up glide
    };

    extern std::map<ViewId, OverlayRec> s_overlays;
    extern std::mutex s_placementMutex;
    extern std::map<ViewId, Placement> s_placementOverrides;
    extern std::map<ViewId, bool> s_recenterRequests;
    extern std::map<ViewId, bool> s_placementResetRequests;
    extern std::map<ViewId, Placement> s_placementSnapshot;
    extern std::uint64_t s_bindGeneration;

    // Every overlay width goes through here. Magelight.json supplies
    // panelWidthM unvalidated, and a zero or NaN width is not a thing the
    // runtime is obliged to survive.
    inline float ClampWidthM(float w)
    {
        if (!std::isfinite(w)) return 1.6f;
        return std::clamp(w, 0.05f, 12.0f);
    }

    void ReleaseRecLocked(OverlayRec& r);
    bool EnsureTexture(OverlayRec& r, int w, int h);
    Placement DefaultPlacementForLayer(int layer);
    bool EnsureOverlay(ViewId id, OverlayRec& r, int layer);
    vr::HmdMatrix34_t HeadRelative(const Placement& p);
    void ApplyPlacement(OverlayRec& r, const Placement& p);
    vr::HmdMatrix34_t LevelMatrix(const float fwd[2], const float pos[3]);
    void TickFollow(OverlayRec& r, const vr::HmdMatrix34_t& hmd, float dt);
    vr::HmdMatrix34_t MatMul(const vr::HmdMatrix34_t& A, const vr::HmdMatrix34_t& B);

    // ── Input.cpp: controller input and held buttons ────────────────────────────
    struct HandInput {
        std::atomic<bool> triggerDown{ false };
        std::atomic<int>  triggerEdges{ 0 };   // +1 per down, -1 per up, consumed by the present thread
        std::atomic<bool> gripEdge{ false };
        std::atomic<bool> menuEdge{ false };   // B/Y
        bool gripWasDown = false;              // game thread only: rising-edge latch
        bool menuWasDown = false;
        std::atomic<int>  stickY_mil{ 0 };     // -1000..1000
        std::atomic<std::uint64_t> stickTick{ 0 };   // GetTickCount64 of the last stick event (staleness guard)
    };

    extern HandInput s_hands[2];
    extern std::atomic<std::uint32_t> s_runtimeHeldMask;
    extern std::atomic<std::uint64_t> s_runtimePressMs[12];
    std::uint32_t PollHeldMask();

    // ── Laser.cpp: aim, hover, beams, the runtime keyboard ──────────────────────
    struct AimCache {
        std::string model;                 // Prop_RenderModelName_String
        vr::HmdMatrix34_t tip{};
        bool haveTip = false;
        bool resolved = false;
        int  recheck = 0;   // frames until the model name is re-queried
    };

    extern AimCache s_aim[2];
    extern ViewId s_kbView;

    // Last hit per hand (present thread): the copy pass stamps the host
    // cursor sprite into that view's texture at (u,v) next frame.
    struct HoverState { ViewId view = 0; float u = 0.0f, v = 0.0f; bool hit = false; };

    extern HoverState s_hover[2];
    void ForgetBeams();
    void ReleaseBeams();
    void TickKeyboard();
    void ClearLaserState();
    void TickLaser(const PresentedFrame& frame);

    // ── Bindings.cpp: controller hotkeys ────────────────────────────────────────
    bool ButtonHeld(const vr::VRControllerState_t& st, std::uint32_t button);
    void TickVRHotkeys();

}  // namespace Magelight::VR
