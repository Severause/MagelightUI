// Magelight UI — Skyrim VR presenter (docs/VR_PRESENTER.md).
//
// STATUS: VR-2 (0.17.1). Compiles against the vendored openvr.h the NG port
// propagates (IVRSystem_017 / IVRCompositor_021 / IVROverlay_016) and
// resolves every interface at runtime from the openvr_api.dll the game
// loaded — no import library, no /DELAYLOAD. VR-1 (detection, binding, the
// probe overlay) field-passed on OpenComposite Unleashed AND SteamVR native
// 2026-09-03; VR-2 submits every visible view as a head-locked overlay
// through the host copy pass. Wired into Magelight.cpp at EnsureRenderInit
// (Init), FrameWork (SubmitFrame), ApplyPendingLifecycle / HibernateIdleViews
// (ReleaseView) and HookPresent's render-death path (Shutdown). Laser input
// (VR-3) is still the TODO block in TickLaser.

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

// The only translation unit that sees OpenVR (design §8).
#include <openvr.h>

#include "MagelightApi4.h"          // DispatchViewAction (game-thread gated action)
#include "../api/MagelightUI_API.h" // kHotkeyAction*

namespace Magelight::VR {

    // ── Runtime binding ─────────────────────────────────────────────────────
    namespace {
        using GetGenericInterfaceFn = void* (*)(const char*, vr::EVRInitError*);
        using IsInterfaceVersionValidFn = bool (*)(const char*);
        using GetInitTokenFn = std::uint32_t (*)();

        std::atomic<int>  s_state{ static_cast<int>(State::Dormant) };
        bool              s_isVR = false;          // REL::Module::IsVR(), latched in Init
        Settings          s_settings;              // Magelight.json "vr"
        DWORD             s_thread = 0;            // the thread Init ran on (== Ultralight thread)
        vr::VROverlayHandle_t s_probe = vr::k_ulOverlayHandleInvalid;   // VR-1: the hidden probe overlay

        vr::IVRSystem*     s_system = nullptr;
        vr::IVRCompositor* s_compositor = nullptr;
        vr::IVROverlay*    s_overlay = nullptr;    // the CLEAN interface (texture submission works)
        vr::IVRRenderModels* s_renderModels = nullptr;   // optional: the controller "tip" aim frame
        std::uint32_t      s_initToken = 0;
        GetGenericInterfaceFn     s_getInterface = nullptr;
        IsInterfaceVersionValidFn s_isVersionValid = nullptr;
        GetInitTokenFn            s_getInitToken = nullptr;

        ID3D11Device*        s_device = nullptr;   // not owned (the host's refs)
        ID3D11DeviceContext* s_context = nullptr;

        const char* kOverlayKeyPrefix = "severause.magelight.view.";

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
            // call pops its "Hit stubbed file" dialog every time (field
            // 2026-09-05). It is recognisable by the factory exports stock
            // openvr_api.dll never has (HmdSystemFactory, VRSystem, ...), so
            // don't touch render models there at all.
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
    }

    // ── Per-view overlay records (Ultralight thread only) ───────────────────
    namespace {
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
        std::map<ViewId, OverlayRec> s_overlays;

        // Placement overrides from the API (any thread) — applied next frame.
        std::mutex s_placementMutex;
        std::map<ViewId, Placement> s_placementOverrides;
        std::map<ViewId, bool>      s_recenterRequests;
        // "reset to the layer default" requests (SetViewVRPlacement(view, nullptr)),
        // drained in SubmitFrame where the layer is known (finding 6).
        std::map<ViewId, bool>      s_placementResetRequests;
        // A thread-safe COPY of each view's effective placement, published by
        // the present thread each frame under s_placementMutex; GetPlacement
        // reads THIS, never s_overlays (which the present thread mutates
        // lock-free) — review 2026-09-05, finding 3.
        std::map<ViewId, Placement> s_placementSnapshot;

        // Bumped on every (re)bind of the OpenVR interfaces. Part of each
        // overlay key so a rebind can never collide with an overlay orphaned
        // inside the runtime by a restart mid-teardown.
        std::uint64_t s_bindGeneration = 0;

        // Every overlay width goes through here. Magelight.json supplies
        // panelWidthM unvalidated, and a zero or NaN width is not a thing the
        // runtime is obliged to survive.
        inline float ClampWidthM(float w)
        {
            if (!std::isfinite(w)) return 1.6f;
            return std::clamp(w, 0.05f, 12.0f);
        }

        void ReleaseRecLocked(OverlayRec& r)
        {
            if (s_overlay && r.handle != vr::k_ulOverlayHandleInvalid) {
                s_overlay->HideOverlay(r.handle);
                s_overlay->ClearOverlayTexture(r.handle);   // release the runtime's reference FIRST
                s_overlay->DestroyOverlay(r.handle);
                r.handle = vr::k_ulOverlayHandleInvalid;
            }
            if (r.srv) { r.srv->Release(); r.srv = nullptr; }
            if (r.rtv) { r.rtv->Release(); r.rtv = nullptr; }
            if (r.tex) { r.tex->Release(); r.tex = nullptr; }
            r.texW = r.texH = 0;
            r.shown = r.placed = false;
        }

        bool EnsureTexture(OverlayRec& r, int w, int h)
        {
            if (r.tex && r.texW == w && r.texH == h) return true;
            if (r.srv) { r.srv->Release(); r.srv = nullptr; }
            if (r.rtv) { r.rtv->Release(); r.rtv = nullptr; }
            if (r.tex) { r.tex->Release(); r.tex = nullptr; }
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(w);
            td.Height = static_cast<UINT>(h);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // sRGB bytes, ColorSpace_Gamma on submit
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            // TODO(VR-2, verify): SteamVR's vrclient copies in-process, so no
            // MISC_SHARED is needed; flip it on if either runtime refuses.
            if (FAILED(s_device->CreateTexture2D(&td, nullptr, &r.tex))) return false;
            if (FAILED(s_device->CreateRenderTargetView(r.tex, nullptr, &r.rtv))) return false;
            if (FAILED(s_device->CreateShaderResourceView(r.tex, nullptr, &r.srv))) return false;
            // Transparent until the copy pass (VR-2) fills it: a fresh D3D
            // texture's contents are undefined, and undefined must never
            // reach the headset.
            const float clear[4] = { 0, 0, 0, 0 };
            s_context->ClearRenderTargetView(r.rtv, clear);
            r.texW = w; r.texH = h;
            return true;
        }

        // Host defaults by z-order tier (design §5): popups closer and smaller,
        // HUD widgets small and up to the right, panels the reading distance.
        Placement DefaultPlacementForLayer(int layer)
        {
            Placement p;
            switch (layer) {
            case 0:  p.mode = Mode::HeadLocked; p.distanceMeters = 1.5f; p.widthMeters = 0.45f; p.heightOffset = 0.30f; break;   // Hud: glued
            case 2:  p.distanceMeters = 1.2f; p.widthMeters = 0.90f; p.heightOffset = -0.10f; break;  // Popup
            case 3:  p.distanceMeters = 1.2f; p.widthMeters = 1.20f; p.heightOffset = 0.0f; break;    // System
            default:   // Panel — the full-page layer, driven by Magelight.json
                p.distanceMeters = s_settings.panelDistanceM;
                p.widthMeters    = s_settings.panelWidthM;
                p.heightOffset   = s_settings.panelHeightOffsetM;
                break;
            }
            // Every non-HUD surface honours the follow setting. A HUD widget
            // is glued to the head by definition and is not a "panel", so it
            // keeps HeadLocked regardless.
            if (layer != 0) {
                p.mode = s_settings.panelFollow ? Mode::LazyFollow : Mode::WorldLocked;
            }
            return p;
        }

        bool EnsureOverlay(ViewId id, OverlayRec& r, int layer)
        {
            if (r.handle != vr::k_ulOverlayHandleInvalid) return true;
            // The key carries the bind generation: if a previous overlay of
            // ours ever survives teardown (a runtime restart mid-destroy),
            // the next CreateOverlay must not collide with its corpse.
            const std::string key = kOverlayKeyPrefix + std::to_string(id) +
                                    "." + std::to_string(s_bindGeneration);
            const std::string name = "Magelight view " + std::to_string(id);
            const auto err = s_overlay->CreateOverlay(key.c_str(), name.c_str(), &r.handle);
            if (err != vr::VROverlayError_None) {
                // LATCHED. This runs per view per frame on a persistent
                // failure, and the logger is flush-on-trace — an unlatched
                // error here is a synchronous disk flush at frame rate, which
                // turns a cosmetic fault into a frame-rate collapse. Mirrors
                // the SetOverlayTexture failure path.
                static int s_logged = 0;
                if (s_logged++ < 4)
                    SKSE::log::error("Magelight VR: CreateOverlay for view {} failed ({}) — "
                                     "logged at most 4x per session", id, static_cast<int>(err));
                r.handle = vr::k_ulOverlayHandleInvalid;
                return false;
            }
            s_overlay->SetOverlayWidthInMeters(r.handle, ClampWidthM(r.placement.widthMeters));
            s_overlay->SetOverlayTextureColorSpace(r.handle, vr::ColorSpace_Gamma);
            s_overlay->SetOverlayAlpha(r.handle, 1.0f);
            // Input is OURS (design §6): no VROverlayInputMethod_Mouse, no
            // runtime laser — OpenComposite would not deliver it anyway.
            s_overlay->SetOverlayInputMethod(r.handle, vr::VROverlayInputMethod_None);
            s_overlay->SetOverlayFlag(r.handle, vr::VROverlayFlags_SortWithNonSceneOverlays, true);
            s_overlay->SetOverlaySortOrder(r.handle, static_cast<std::uint32_t>(10 * (layer + 1)));
            SKSE::log::info("Magelight VR: overlay created for view {} ({}, layer {}, {:.2f}m wide at {:.2f}m)",
                id, key, layer, r.placement.widthMeters, r.placement.distanceMeters);
            return true;
        }

        // Head-relative placement matrix: overlay centre `distance` metres
        // along the HMD's -Z, `heightOffset` below eye height, facing the
        // HMD (identity rotation in HMD space — the quad's +Z looks back at
        // the viewer).
        vr::HmdMatrix34_t HeadRelative(const Placement& p)
        {
            vr::HmdMatrix34_t m{};
            m.m[0][0] = 1.0f; m.m[1][1] = 1.0f; m.m[2][2] = 1.0f;
            m.m[0][3] = 0.0f;
            m.m[1][3] = p.heightOffset;
            m.m[2][3] = -p.distanceMeters;
            return m;
        }

        // TODO(VR-2): WorldLocked — read the HMD pose (GetLastPoses, standing
        // universe), compose HeadRelative into it with the yaw only (no roll/
        // pitch bake), SetOverlayTransformAbsolute, remember `world` for the
        // laser. HeadLocked uses SetOverlayTransformTrackedDeviceRelative and
        // reconstructs `world` every frame from the HMD pose for the laser.
        void ApplyPlacement(OverlayRec& r, const Placement& p)
        {
            r.placement = p;
            s_overlay->SetOverlayWidthInMeters(r.handle, ClampWidthM(p.widthMeters));
            switch (p.mode) {
            case Mode::HeadLocked: {
                const auto m = HeadRelative(p);
                s_overlay->SetOverlayTransformTrackedDeviceRelative(r.handle, vr::k_unTrackedDeviceIndex_Hmd, &m);
                break;
            }
            case Mode::LazyFollow:
            case Mode::WorldLocked:
                r.placed = false;   // TickFollow places it from the live HMD pose next frame
                r.following = false;
                return;
            case Mode::HandLocked:
                // TODO(later)
                break;
            }
            r.placed = true;
        }

        // ── Lazy follow (present thread) ────────────────────────────────
        // A LEVEL panel: forward = the HMD's forward projected onto XZ, position
        // = head + forward*distance + heightOffset up. Absolute transform in the
        // compositor's space; the same matrix is what the laser hits.
        constexpr float kFollowRate     = 7.0f;      // 1/s; ~150 ms time constant
        constexpr float kSnapAngleCos   = 0.99966f;  // cos 1.5 deg
        constexpr float kSnapDistM      = 0.02f;

        // The gaze may leave the panel's EDGE by followAngleDeg before the
        // glide starts, so the deadzone grows with the panel instead of being
        // a fixed cone around its centre — look at a wide page's far column
        // and it stays put, which a centre-relative threshold cannot do.
        float FollowCosThreshold(const Placement& p)
        {
            const float dist = std::max(0.2f, p.distanceMeters);
            const float halfDeg = std::atan2(std::max(0.05f, p.widthMeters * 0.5f), dist) * 57.2957795f;
            const float fromEdge = s_settings.followAngleDeg + halfDeg;
            const float deg = std::clamp(std::max(fromEdge, s_settings.followMinDeg), 5.0f, 110.0f);
            return std::cos(deg * 0.01745329252f);
        }

        bool LazyTarget(const vr::HmdMatrix34_t& hmd, const Placement& p, float fwd[2], float pos[3])
        {
            float fx = -hmd.m[0][2], fz = -hmd.m[2][2];
            const float n = std::sqrt(fx * fx + fz * fz);
            if (n < 1e-3f) return false;   // looking straight up/down: keep the old heading
            fx /= n; fz /= n;
            fwd[0] = fx; fwd[1] = fz;
            pos[0] = hmd.m[0][3] + fx * p.distanceMeters;
            pos[1] = hmd.m[1][3] + p.heightOffset;
            pos[2] = hmd.m[2][3] + fz * p.distanceMeters;
            return true;
        }

        vr::HmdMatrix34_t LevelMatrix(const float fwd[2], const float pos[3])
        {
            // Overlay +Z faces the viewer: Z = -forward (back toward the head), Y up, X = Y x Z.
            const float dx = fwd[0], dz = fwd[1];
            vr::HmdMatrix34_t M{};
            M.m[0][0] = -dz; M.m[1][0] = 0.0f; M.m[2][0] = dx;    // X
            M.m[0][1] = 0.0f; M.m[1][1] = 1.0f; M.m[2][1] = 0.0f;  // Y
            M.m[0][2] = -dx; M.m[1][2] = 0.0f; M.m[2][2] = -dz;   // Z
            M.m[0][3] = pos[0]; M.m[1][3] = pos[1]; M.m[2][3] = pos[2];
            return M;
        }

        void TickFollow(OverlayRec& r, const vr::HmdMatrix34_t& hmd, float dt)
        {
            float tf[2], tp[3];
            if (!LazyTarget(hmd, r.placement, tf, tp)) {
                if (!r.placed) return;   // can't place yet
                tf[0] = r.fwd[0]; tf[1] = r.fwd[1];   // keep heading, still track position
                tp[0] = hmd.m[0][3] + tf[0] * r.placement.distanceMeters;
                tp[1] = hmd.m[1][3] + r.placement.heightOffset;
                tp[2] = hmd.m[2][3] + tf[1] * r.placement.distanceMeters;
            }
            bool write = false;
            if (!r.placed) {
                r.fwd[0] = tf[0]; r.fwd[1] = tf[1];
                r.pos[0] = tp[0]; r.pos[1] = tp[1]; r.pos[2] = tp[2];
                r.placed = true; r.following = false; write = true;
            } else if (r.placement.mode == Mode::LazyFollow) {
                const float cosang = r.fwd[0] * tf[0] + r.fwd[1] * tf[1];
                const float ddx = r.pos[0] - tp[0], ddy = r.pos[1] - tp[1], ddz = r.pos[2] - tp[2];
                const float dist = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                if (!r.following &&
                    (cosang < FollowCosThreshold(r.placement) ||
                     dist > std::max(0.05f, s_settings.followDistM)))
                    r.following = true;
                if (r.following) {
                    const float k = 1.0f - std::exp(-std::clamp(dt, 0.0f, 0.1f) * kFollowRate);
                    float nx = r.fwd[0] + (tf[0] - r.fwd[0]) * k, nz = r.fwd[1] + (tf[1] - r.fwd[1]) * k;
                    const float nn = std::sqrt(nx * nx + nz * nz);
                    if (nn > 1e-4f) { r.fwd[0] = nx / nn; r.fwd[1] = nz / nn; }
                    for (int i = 0; i < 3; ++i) r.pos[i] += (tp[i] - r.pos[i]) * k;
                    const float c2 = r.fwd[0] * tf[0] + r.fwd[1] * tf[1];
                    const float d2x = r.pos[0] - tp[0], d2y = r.pos[1] - tp[1], d2z = r.pos[2] - tp[2];
                    if (c2 > kSnapAngleCos && std::sqrt(d2x * d2x + d2y * d2y + d2z * d2z) < kSnapDistM) {
                        r.fwd[0] = tf[0]; r.fwd[1] = tf[1];
                        r.pos[0] = tp[0]; r.pos[1] = tp[1]; r.pos[2] = tp[2];
                        r.following = false;
                    }
                    write = true;
                }
            }
            if (write) {
                r.world = LevelMatrix(r.fwd, r.pos);
                if (s_overlay && s_compositor && r.handle != vr::k_ulOverlayHandleInvalid)
                    s_overlay->SetOverlayTransformAbsolute(r.handle, s_compositor->GetTrackingSpace(), &r.world);
            }
        }
    }

    // ── Input edges (game thread writes, present thread reads) ──────────────
    namespace {
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
        HandInput s_hands[2];   // 0 = left, 1 = right (physical), resolved via IsLeftHandedMode at use

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

    // ── Per-frame pose snapshot (present thread) ────────────────────────────
    namespace {
        vr::TrackedDevicePose_t s_frameRender[vr::k_unMaxTrackedDeviceCount];
        bool  s_frameHmdUsable = false;
        float s_frameDt = 0.0f;
        std::chrono::steady_clock::time_point s_lastFrameTp{};
    }

    // ── Laser (present thread) ──────────────────────────────────────────────
    namespace {
        // Ray/quad intersection against a view's authored world transform.
        // Returns uv in [0,1] (u right, v DOWN — view space) and the hit distance.
        bool IntersectQuad(const vr::HmdMatrix34_t& quad, float widthM, float heightM,
                           const vr::HmdMatrix34_t& ctrl, float& u, float& v, float& dist)
        {
            // Quad basis (columns of the rotation): X right, Y up, Z out of the panel.
            const float ox = quad.m[0][3], oy = quad.m[1][3], oz = quad.m[2][3];
            const float xx = quad.m[0][0], xy = quad.m[1][0], xz = quad.m[2][0];
            const float yx = quad.m[0][1], yy = quad.m[1][1], yz = quad.m[2][1];
            const float nx = quad.m[0][2], ny = quad.m[1][2], nz = quad.m[2][2];
            // Controller ray: origin = translation, direction = -Z of the pose.
            const float px = ctrl.m[0][3], py = ctrl.m[1][3], pz = ctrl.m[2][3];
            const float dx = -ctrl.m[0][2], dy = -ctrl.m[1][2], dz = -ctrl.m[2][2];
            const float denom = dx * nx + dy * ny + dz * nz;
            if (std::fabs(denom) < 1e-5f) return false;
            const float t = ((ox - px) * nx + (oy - py) * ny + (oz - pz) * nz) / denom;
            if (t <= 0.0f) return false;
            const float hx = px + dx * t - ox, hy = py + dy * t - oy, hz = pz + dz * t - oz;
            const float lx = hx * xx + hy * xy + hz * xz;   // metres along the quad's X
            const float ly = hx * yx + hy * yy + hz * yz;   // metres along Y (up)
            if (std::fabs(lx) > widthM * 0.5f || std::fabs(ly) > heightM * 0.5f) return false;
            u = lx / widthM + 0.5f;
            v = 0.5f - ly / heightM;
            dist = t;
            return true;
        }

        // ── VR-3 laser helpers (verified plan; docs/VR_PRESENTER.md §6) ──
        // 3x4 [R|t] multiply with the implicit bottom row [0 0 0 1].
        vr::HmdMatrix34_t MatMul(const vr::HmdMatrix34_t& A, const vr::HmdMatrix34_t& B)
        {
            vr::HmdMatrix34_t C{};
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j)
                    C.m[i][j] = A.m[i][0] * B.m[0][j] + A.m[i][1] * B.m[1][j] + A.m[i][2] * B.m[2][j];
                C.m[i][3] = A.m[i][0] * B.m[0][3] + A.m[i][1] * B.m[1][3] + A.m[i][2] * B.m[2][3] + A.m[i][3];
            }
            return C;
        }

        bool PoseUsable(const vr::TrackedDevicePose_t& p)
        {
            // Running_OutOfRange is a NORMAL, recoverable state (you stepped
            // outside the play space, a base station lost you for a moment).
            // The pose it carries is still valid — bPoseIsValid says so — and
            // demanding Running_OK meant a panel never got placed and the
            // laser died outright for the duration. Everything else
            // (Uninitialised, calibrating, fallback-rotation-only) stays out.
            if (!p.bPoseIsValid || !p.bDeviceIsConnected) return false;
            return p.eTrackingResult == vr::TrackingResult_Running_OK ||
                   p.eTrackingResult == vr::TrackingResult_Running_OutOfRange;
        }

        // ── Aim frame ────────────────────────────────────────────────
        // Preferred: the controller's render-model "tip" component. OpenVR
        // defines its local frame with -Z out of the surface, which is exactly
        // the ray convention IntersectQuad uses, and it is per-controller
        // correct — a Touch, an Index and a Vive wand all hold differently, so
        // one hard-coded tilt can only ever suit one of them. Resolved once per
        // device (the component is static; the model name changes only if the
        // controller does) and cached. Fallback when a runtime serves no render
        // models or the model has no tip: the fixed local-X pitch below.
        struct AimCache {
            std::string model;                 // Prop_RenderModelName_String
            vr::HmdMatrix34_t tip{};
            bool haveTip = false;
            bool resolved = false;
            int  recheck = 0;   // frames until the model name is re-queried
        };
        AimCache s_aim[2];

        void ResolveAim(int hand, vr::TrackedDeviceIndex_t idx, const vr::VRControllerState_t& st)
        {
            if (!s_system) return;
            // The property query below is an IPC round-trip to the runtime and
            // it sat ABOVE the cache check, so it ran twice a frame forever to
            // answer a question whose answer changes only when a controller is
            // swapped. Re-ask about once a second; the cached aim is used in
            // between. (Audit finding B5, 2026-09-04.)
            AimCache& cached = s_aim[hand];
            if (cached.resolved && cached.recheck > 0) { --cached.recheck; return; }
            // Render-model names are short; k_unMaxPropertyStringSize is 32 KB
            // and has no business on the present thread's stack.
            char name[256]{};
            vr::ETrackedPropertyError perr = vr::TrackedProp_Success;
            s_system->GetStringTrackedDeviceProperty(idx, vr::Prop_RenderModelName_String,
                                                     name, sizeof(name), &perr);
            const std::string model = (perr == vr::TrackedProp_Success) ? name : std::string();
            AimCache& a = s_aim[hand];
            if (a.resolved && a.model == model) { a.recheck = 90; return; }   // unchanged
            a = AimCache{};
            a.model = model;
            a.resolved = true;
            a.recheck = 90;
            if (model.empty() || !s_renderModels || !s_settings.aimUseTip) {
                SKSE::log::info("Magelight VR: hand {} aim = fixed pitch {:.1f} deg (model '{}')",
                    hand, s_settings.aimPitchDeg, model);
                return;
            }
            if (!s_renderModels->RenderModelHasComponent(model.c_str(), vr::k_pch_Controller_Component_Tip)) {
                SKSE::log::info("Magelight VR: hand {} model '{}' has no tip component — fixed pitch {:.1f} deg",
                    hand, model, s_settings.aimPitchDeg);
                return;
            }
            vr::RenderModel_ComponentState_t cs{};
            vr::RenderModel_ControllerMode_State_t mode{};
            if (s_renderModels->GetComponentState(model.c_str(), vr::k_pch_Controller_Component_Tip,
                                                  &st, &mode, &cs)) {
                a.tip = cs.mTrackingToComponentLocal;   // -Z out of the surface
                a.haveTip = true;
                SKSE::log::info("Magelight VR: hand {} aim = render-model tip of '{}'", hand, model);
            } else {
                SKSE::log::info("Magelight VR: hand {} tip state unavailable for '{}' — fixed pitch {:.1f} deg",
                    hand, model, s_settings.aimPitchDeg);
            }
        }

        vr::HmdMatrix34_t ApplyLocalPitch(const vr::HmdMatrix34_t& m, float deg)
        {
            if (deg == 0.0f) return m;
            const float r = deg * 0.01745329252f, c = std::cos(r), sn = std::sin(r);
            vr::HmdMatrix34_t o = m;   // X (col 0) and translation (col 3) unchanged
            for (int i = 0; i < 3; ++i) {
                const float y = m.m[i][1], z = m.m[i][2];
                o.m[i][1] =  c * y + sn * z;
                o.m[i][2] = -sn * y + c * z;
            }
            return o;
        }

        // Trigger click LEVEL per hand, present thread only (M6: level, not the
        // net edge counter which loses a press+release inside one frame).
        bool s_lastTriggerDown[2] = { false, false };
        bool s_gripWas[2] = { false, false };            // polled grip level, rising-edge latch
        bool s_gripSeeded[2] = { false, false };         // latch seeded from the live level on UI-mode entry
        int  s_lastPx[2] = { 0, 0 }, s_lastPy[2] = { 0, 0 };   // last hit pixel per hand (the UP lands here)

        // VR submit accounting. Logged periodically while a panel is up so a
        // field log SAYS whether the overlay path is doing per-frame work,
        // instead of us inferring it from symptoms.
        std::uint64_t s_vrFrames = 0, s_vrSubmits = 0, s_vrReported = 0;


        // Grab-and-move state (present thread; the request comes from the page).
        std::atomic<std::uint64_t> s_dragRequest{ 0 };   // view the page asked to drag (0 = none)
        ViewId s_dragView = 0;                           // active drag
        int    s_dragHand = -1;
        float  s_dragDist = 1.1f;

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
        // bystander view's action (review 2026-09-05, finding 1).
        std::uint32_t               s_bindEpoch = 0;   // guarded by s_bindMutex
        // Present-thread only: seed the capture masks on the first tick after
        // arming. The 0xFFFFFFFF "suppress everything currently held" seed is
        // written by the OWNER of these arrays (the tick), never by
        // SetButtonListener on the game thread — that cross-thread write raced
        // the tick's own writes (review 2026-09-05, finding 7).
        bool                        s_edgeSeedPending = false;  // guarded by s_bindMutex
        std::map<ViewId, bool>      s_bindWas;       // present thread: rising-edge latch per view
        std::map<ViewId, std::uint32_t> s_bindWasEpoch;  // present thread: bind epoch the latch last reset for
        std::map<ViewId, std::uint64_t> s_bindLastTick;  // present thread: PER-VIEW debounce (finding 5)
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
        // pressed (→ reported bare). Field 2026-09-05: without this, pressing
        // Grip to start "Grip + A" captured a bare Grip immediately.
        std::uint32_t s_edgePending[2] = { 0, 0 };
        // The chord vocabulary, in the order a chord's MODIFIER is reported
        // when more than one other button is held (Grip and Trigger are what
        // people naturally hold while pressing a face button).
        constexpr std::uint32_t kEdgeCodes[] = { 2, 33, 1, 7, 32, 35 };

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

        // ── Virtual keyboard (VR-4) ───────────────────────────────────
        ViewId s_kbView = 0;          // view whose text field has focus (0 = none)
        bool   s_kbShown = false;     // the runtime's keyboard is up for it
        float s_scrollAccum[2] = { 0.0f, 0.0f };         // thumbstick -> wheel notches
        constexpr float kStickDeadzone = 0.30f;
        constexpr float kScrollNotchesPerSec = 6.0f;     // at full deflection
        constexpr std::uint64_t kStickStaleMs = 250;     // no stick event this long = centred

        // Last hit per hand (present thread): the copy pass stamps the host
        // cursor sprite into that view's texture at (u,v) next frame.
        struct HoverState { ViewId view = 0; float u = 0.0f, v = 0.0f; bool hit = false; };
        HoverState s_hover[2];

        // ── Beam overlays (one per hand, SESSION-GLOBAL like the probe) ──
        // A static 256x2 straight-alpha gradient texture set ONCE (never
        // recreated while the runtime references it — recreating one it still
        // holds is a known teardown fault); per frame only width + an absolute transform that
        // billboards the strip toward the HMD so it is never seen edge-on.
        // Released in Shutdown only, never in ReleaseView.
        vr::VROverlayHandle_t s_beam[2] = { vr::k_ulOverlayHandleInvalid, vr::k_ulOverlayHandleInvalid };
        bool                  s_beamShown[2] = { false, false };
        ID3D11Texture2D*      s_beamTex = nullptr;
        // Wider texture = THINNER beam: the overlay's height is
        // width(metres) * texH/texW, so 512x2 is half the thickness 256x2 gave
        // (~0.8 cm at 2 m). White, fading toward the tip — the thin pale beam
        // the runtime's own pointer uses, rather than a fat coloured one.
        constexpr int         kBeamTexW = 512, kBeamTexH = 2;
        constexpr float       kBeamIdleLen = 2.0f;   // metres when nothing is hit

        bool EnsureBeamTexture()
        {
            if (s_beamTex || !s_device) return s_beamTex != nullptr;
            static std::uint32_t px[kBeamTexW * kBeamTexH];
            for (int y = 0; y < kBeamTexH; ++y)
                for (int x = 0; x < kBeamTexW; ++x) {
                    const float t = static_cast<float>(x) / (kBeamTexW - 1);
                    const std::uint32_t a = static_cast<std::uint32_t>(std::lround(200.0f * (1.0f - t * t)));
                    px[y * kBeamTexW + x] = (a << 24) | (0xFFu << 16) | (0xFFu << 8) | 0xFFu;   // straight BGRA: white
                }
            D3D11_TEXTURE2D_DESC td{};
            td.Width = kBeamTexW; td.Height = kBeamTexH; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sub{ px, kBeamTexW * sizeof(std::uint32_t), 0 };
            if (FAILED(s_device->CreateTexture2D(&td, &sub, &s_beamTex))) { s_beamTex = nullptr; return false; }
            return true;
        }

        bool EnsureBeam(int hand)
        {
            if (s_beam[hand] != vr::k_ulOverlayHandleInvalid) return true;
            if (!s_overlay || !EnsureBeamTexture()) return false;
            const char* key  = hand ? "severause.magelight.beam.right" : "severause.magelight.beam.left";
            const char* name = hand ? "Magelight beam R" : "Magelight beam L";
            const auto err = s_overlay->CreateOverlay(key, name, &s_beam[hand]);
            if (err != vr::VROverlayError_None) {
                s_beam[hand] = vr::k_ulOverlayHandleInvalid;
                static int logged = 0;
                if (logged++ < 2) SKSE::log::error("Magelight VR: beam overlay {} failed ({})", key, static_cast<int>(err));
                return false;
            }
            vr::Texture_t tex{ s_beamTex, vr::TextureType_DirectX, vr::ColorSpace_Gamma };
            s_overlay->SetOverlayTexture(s_beam[hand], &tex);   // once; static texture
            s_overlay->SetOverlayWidthInMeters(s_beam[hand], kBeamIdleLen);
            s_overlay->SetOverlayAlpha(s_beam[hand], std::clamp(s_settings.beamAlpha, 0.05f, 1.0f));
            s_overlay->SetOverlayInputMethod(s_beam[hand], vr::VROverlayInputMethod_None);
            s_overlay->SetOverlayFlag(s_beam[hand], vr::VROverlayFlags_SortWithNonSceneOverlays, true);
            s_overlay->SetOverlaySortOrder(s_beam[hand], 100);
            SKSE::log::info("Magelight VR: beam overlay created ({})", key);
            return true;
        }

        void HideBeam(int hand)
        {
            if (s_beamShown[hand] && s_overlay && s_beam[hand] != vr::k_ulOverlayHandleInvalid)
                s_overlay->HideOverlay(s_beam[hand]);
            s_beamShown[hand] = false;
        }

        // Strip from the controller along the (pitched) ray, `len` metres,
        // billboarded toward the HMD. X = ray, Z = toward head (orthogonal to
        // the ray), Y = Z x X. Absolute transform in the compositor's space.
        void UpdateBeam(int hand, const vr::HmdMatrix34_t& ctrl, const vr::HmdMatrix34_t& hmd, float len)
        {
            if (!s_settings.beam) { HideBeam(hand); return; }
            if (!EnsureBeam(hand)) return;
            const float px = ctrl.m[0][3], py = ctrl.m[1][3], pz = ctrl.m[2][3];
            const float dx = -ctrl.m[0][2], dy = -ctrl.m[1][2], dz = -ctrl.m[2][2];
            const float cx = px + dx * len * 0.5f, cy = py + dy * len * 0.5f, cz = pz + dz * len * 0.5f;
            float vx = hmd.m[0][3] - cx, vy = hmd.m[1][3] - cy, vz = hmd.m[2][3] - cz;
            const float along = vx * dx + vy * dy + vz * dz;
            vx -= dx * along; vy -= dy * along; vz -= dz * along;
            const float n = std::sqrt(vx * vx + vy * vy + vz * vz);
            float zx, zy, zz;
            if (n < 1e-4f) { zx = ctrl.m[0][1]; zy = ctrl.m[1][1]; zz = ctrl.m[2][1]; }   // degenerate: controller Y
            else { zx = vx / n; zy = vy / n; zz = vz / n; }
            // Y = Z x X
            const float yx = zy * dz - zz * dy, yy = zz * dx - zx * dz, yz = zx * dy - zy * dx;
            vr::HmdMatrix34_t M{};
            M.m[0][0] = dx; M.m[1][0] = dy; M.m[2][0] = dz;
            M.m[0][1] = yx; M.m[1][1] = yy; M.m[2][1] = yz;
            M.m[0][2] = zx; M.m[1][2] = zy; M.m[2][2] = zz;
            M.m[0][3] = cx; M.m[1][3] = cy; M.m[2][3] = cz;
            // `len` is the ray-hit distance, and IntersectQuad only guarantees
            // it is > 0 — as the hand approaches the panel plane it tends to
            // zero, and reaching THROUGH the panel makes it denormal-small.
            // OpenVR wants a positive width and a runtime is entitled to do
            // anything with a zero one; a beam that thin is invisible anyway.
            // Suspected in the "reaching out past a threshold freezes the
            // game" report (2026-09-04) — the threshold is the panel distance.
            if (!std::isfinite(len) || len < 0.02f) { HideBeam(hand); return; }
            s_overlay->SetOverlayWidthInMeters(s_beam[hand], len);
            s_overlay->SetOverlayTransformAbsolute(s_beam[hand], s_compositor->GetTrackingSpace(), &M);
            if (!s_beamShown[hand]) { s_overlay->ShowOverlay(s_beam[hand]); s_beamShown[hand] = true; }
        }

        // Runtime restart (init token changed): every handle is dead. Forget
        // them; the texture is ours and survives. Called from SubmitFrame.
        void ForgetBeams()
        {
            for (int h = 0; h < 2; ++h) { s_beam[h] = vr::k_ulOverlayHandleInvalid; s_beamShown[h] = false; }
        }

        void ReleaseBeams()
        {
            for (int h = 0; h < 2; ++h) {
                if (s_overlay && s_beam[h] != vr::k_ulOverlayHandleInvalid) {
                    s_overlay->HideOverlay(s_beam[h]);
                    s_overlay->ClearOverlayTexture(s_beam[h]);
                    s_overlay->DestroyOverlay(s_beam[h]);
                }
                s_beam[h] = vr::k_ulOverlayHandleInvalid;
                s_beamShown[h] = false;
            }
            if (s_beamTex) { s_beamTex->Release(); s_beamTex = nullptr; }
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
                    // before it can fire. Reseeding only the rebound view left
                    // bystander views to fire on a held-through chord.
                    seenEpoch = epoch;
                    was = down;
                    continue;
                }
                if (down && !was) {
                    // Per-view debounce: a single global timestamp let one
                    // view's fire swallow another view's distinct hotkey within
                    // 250ms (finding 5).
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

        // ── SteamVR virtual keyboard ──────────────────────────────────
        void TickKeyboard()
        {
            if (!s_overlay) return;
            if (!s_settings.runtimeKeyboard) {
                static bool told = false;
                if (!told && s_kbView) {
                    told = true;
                    SKSE::log::info("Magelight VR: a text field has focus; the runtime keyboard is OFF by default "
                                    "(it steals input focus and stalls the game) — type on the keyboard, or on the "
                                    "runtime's own if it synthesises scancodes");
                }
                if (s_kbShown) { s_overlay->HideKeyboard(); s_kbShown = false; }
                return;
            }
            // Raise / drop the runtime keyboard to follow the page's text focus.
            const ViewId want = s_kbView;
            if (want && !s_kbShown) {
                auto it = s_overlays.find(want);
                if (it != s_overlays.end() && it->second.handle != vr::k_ulOverlayHandleInvalid) {
                    // MINIMAL MODE (true): the runtime sends one
                    // VREvent_KeyboardCharInput per keystroke instead of
                    // keeping its own text buffer and handing the whole string
                    // over at Done. We are typing into a live web page, so we
                    // want the keystrokes (field 2026-09-03: with minimal mode
                    // off, SteamVR's keyboard opened and produced nothing).
                    const auto err = s_overlay->ShowKeyboardForOverlay(
                        it->second.handle, vr::k_EGamepadTextInputModeNormal,
                        vr::k_EGamepadTextInputLineModeSingleLine, "Magelight", 256, "", true, 0);
                    if (err == vr::VROverlayError_None) {
                        s_kbShown = true;
                        SKSE::log::info("Magelight VR: runtime keyboard shown for view {}", want);
                    } else {
                        // OpenComposite has no overlay keyboard to raise; on that runtime the
                        // host's own on-panel keyboard (and OCU's WM_OC_CHAR window message) carry text.
                        static bool warned = false;
                        if (!warned) {
                            warned = true;
                            SKSE::log::info("Magelight VR: runtime keyboard unavailable ({}) — relying on the runtime's own (OCU) path",
                                static_cast<int>(err));
                        }
                        s_kbView = 0;
                    }
                }
            } else if (!want && s_kbShown) {
                s_overlay->HideKeyboard();
                s_kbShown = false;
            }
            if (!s_kbShown) return;
            // Drain keyboard events off the focused view's overlay.
            auto it = s_overlays.find(s_kbView);
            if (it == s_overlays.end()) { s_overlay->HideKeyboard(); s_kbShown = false; s_kbView = 0; return; }
            // BOUNDED: this runs on the present thread, and raising the
            // keyboard produced a flood of focus events in the field. An
            // unbounded drain here is a frame stall.
            vr::VREvent_t ev{};
            int drained = 0;
            while (drained++ < 64 && s_overlay->PollNextOverlayEvent(it->second.handle, &ev, sizeof(ev))) {
                if (ev.eventType == vr::VREvent_KeyboardCharInput) {
                    char buf[9]{};
                    std::memcpy(buf, ev.data.keyboard.cNewInput, 8);
                    wchar_t wide[16]{};
                    const int n = MultiByteToWideChar(CP_UTF8, 0, buf, -1, wide, 15);
                    for (int i = 0; i < n && wide[i]; ++i) {
                        if (wide[i] == L'\b') {   // the runtime keyboard's backspace
                            Magelight::QueueSyntheticInput(WM_KEYDOWN, VK_BACK, 0);
                            Magelight::QueueSyntheticInput(WM_KEYUP, VK_BACK, 0);
                        } else if (wide[i] >= 0x20 || wide[i] == L'\t' || wide[i] == L'\r') {
                            Magelight::QueueSyntheticInput(WM_CHAR, static_cast<std::uintptr_t>(wide[i]), 0);
                        }
                    }
                } else if (ev.eventType == vr::VREvent_KeyboardDone) {
                    // Full-mode fallback: the runtime kept the text itself.
                    char text[256]{};
                    const std::uint32_t n = s_overlay->GetKeyboardText(text, sizeof(text));
                    if (n > 0) {
                        wchar_t wide[256]{};
                        const int wn = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 255);
                        for (int i = 0; i < wn && wide[i]; ++i)
                            if (wide[i] >= 0x20 || wide[i] == L'\t' || wide[i] == L'\r')
                                Magelight::QueueSyntheticInput(WM_CHAR, static_cast<std::uintptr_t>(wide[i]), 0);
                        SKSE::log::info("Magelight VR: runtime keyboard done — {} byte(s) of text delivered", n);
                    }
                    s_kbShown = false; s_kbView = 0;
                    break;
                } else if (ev.eventType == vr::VREvent_KeyboardClosed) {
                    s_kbShown = false;
                    s_kbView = 0;
                    SKSE::log::info("Magelight VR: runtime keyboard closed");
                    break;
                }   // every other overlay event (focus churn, process
                    // connect, chaperone) is not ours to act on
            }
        }

        void ClearLaserState()
        {
            for (int h = 0; h < 2; ++h) {
                HideBeam(h); s_hover[h] = HoverState{}; s_lastTriggerDown[h] = false; s_scrollAccum[h] = 0.0f;
                s_gripWas[h] = false; s_gripSeeded[h] = false;
                s_dragRequest.store(0); s_dragView = 0; s_dragHand = -1;
                s_hands[h].stickY_mil.store(0); s_hands[h].gripEdge.store(false);
            }
            if (s_kbShown && s_overlay) s_overlay->HideKeyboard();
            s_kbShown = false; s_kbView = 0;
        }

        // Hover + click on the panel overlays (VR-3 first cut: no beam, no
        // scroll). One pose snapshot; reconstruct each panel's world from the
        // live HMD pose (B1); per hand, intersect topmost-first and inject the
        // in-view pixel through the host's synthetic-input bridge. Present
        // thread; touches no engine state (buttons already came from the sink).
        void TickLaser(const PresentedFrame& frame)
        {
            if (!s_system || !s_compositor || !s_overlay) return;
            if (!s_frameHmdUsable) return;   // snapshot taken in SubmitFrame; no head pose -> nothing to hit
            const vr::TrackedDevicePose_t* render = s_frameRender;
            const vr::TrackedDevicePose_t& hmd = render[vr::k_unTrackedDeviceIndex_Hmd];

            // B1: HeadLocked panels (runtime-relative transform) get their world
            // reconstructed = HMD * relative; lazy/world panels already own theirs.
            for (const PresentedView& pv : frame) {
                if (pv.isInspector || pv.clickThrough || !pv.ul) continue;
                auto it = s_overlays.find(pv.id);
                if (it == s_overlays.end() || !it->second.shown) continue;
                if (it->second.placement.mode == Mode::HeadLocked)
                    it->second.world = MatMul(hmd.mDeviceToAbsoluteTracking, HeadRelative(it->second.placement));
            }

            for (int hand = 0; hand < 2; ++hand) {
                const auto role = (hand == 1) ? vr::TrackedControllerRole_RightHand
                                              : vr::TrackedControllerRole_LeftHand;
                const vr::TrackedDeviceIndex_t idx = s_system->GetTrackedDeviceIndexForControllerRole(role);
                if (idx == vr::k_unTrackedDeviceIndexInvalid || idx >= vr::k_unMaxTrackedDeviceCount) {  // B4
                    s_lastTriggerDown[hand] = false;   // controller gone: forget its click state
                    s_hover[hand].hit = false;
                    HideBeam(hand);
                    continue;
                }
                const vr::TrackedDevicePose_t& ctrlPose = render[idx];
                if (!PoseUsable(ctrlPose)) { s_lastTriggerDown[hand] = false; s_hover[hand].hit = false; HideBeam(hand); continue; }

                // Controller state straight from the runtime (the primitive the
                // game's own BSOpenVRControllerDevice polls): legacy mapping
                // Axis0 = stick/pad, Axis1 = trigger (0..1), Axis2 = grip on
                // Touch/Index; k_EButton_Grip for wands. The engine's ButtonEvent
                // stream for these analog axes is not a usable level source
                // (0.17.7 field: still one click per frame), so the laser no
                // longer depends on it — the sink only swallows those events.
                vr::VRControllerState_t st{};
                bool polled = false, trig = false, gripNow = false; float stickY = 0.0f;
                if (s_system->GetControllerState(idx, &st, sizeof(st))) {
                    polled = true;
                    trig = (st.rAxis[1].x > 0.5f) ||
                           ((st.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0);
                    gripNow = ((st.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0) ||
                              (st.rAxis[2].x > 0.5f);
                    stickY = st.rAxis[0].y;
                } else {
                    static int s_pollWarned = 0;
                    if (s_pollWarned++ < 2)
                        SKSE::log::warn("Magelight VR: GetControllerState failed for hand {} (device {}) — falling back to engine levels", hand, idx);
                }
                // Aim frame: the render-model tip where the controller has one,
                // the fixed pitch otherwise. Resolved once per device.
                ResolveAim(hand, idx, st);
                const vr::HmdMatrix34_t ctrl =
                    s_aim[hand].haveTip
                        ? MatMul(ctrlPose.mDeviceToAbsoluteTracking, s_aim[hand].tip)
                        : ApplyLocalPitch(ctrlPose.mDeviceToAbsoluteTracking, s_settings.aimPitchDeg);

                // Grip rising edge: bring every non-HUD panel back in front of you.
                // A grip already held when UI mode opened is NOT an edge — it is
                // the modifier the user is holding for a controller binding
                // (field 2026-09-03: opening with grip+A recentred on every open).
                {
                    if (!s_gripSeeded[hand]) { s_gripWas[hand] = gripNow; s_gripSeeded[hand] = true; }
                    bool edge;
                    if (polled) { edge = gripNow && !s_gripWas[hand]; s_gripWas[hand] = gripNow; }
                    else edge = s_hands[hand].gripEdge.exchange(false);
                    if (edge) {
                        for (auto& [id, r] : s_overlays)
                            if (r.shown && r.placement.mode != Mode::HeadLocked) { r.placed = false; r.following = false; }
                        SKSE::log::info("Magelight VR: grip (hand {}) — panels recentred", hand);
                    }
                }

                // M5: topmost-first (reverse z-order), first ray hit wins —
                // matches the host hitTest so the pixel re-resolves to this view.
                bool hit = false; float bestU = 0, bestV = 0, bestDist = 0;
                const PresentedView* target = nullptr;
                for (auto rit = frame.rbegin(); rit != frame.rend(); ++rit) {
                    const PresentedView& pv = *rit;
                    if (pv.isInspector || pv.clickThrough || !pv.ul || pv.w <= 0 || pv.h <= 0) continue;
                    auto it = s_overlays.find(pv.id);
                    if (it == s_overlays.end() || !it->second.shown || it->second.texW <= 0) continue;
                    OverlayRec& r = it->second;
                    // Clamp to the SAME width the overlay is displayed at
                    // (SetOverlayWidthInMeters(ClampWidthM(...))) — the raw
                    // panelWidthM is unvalidated, so an out-of-range or NaN
                    // value made the click rectangle diverge from the visible
                    // panel (review 2026-09-05, finding 4).
                    const float widthM  = ClampWidthM(r.placement.widthMeters);
                    const float heightM = widthM * static_cast<float>(r.texH) / static_cast<float>(r.texW);  // M4
                    float u, v, dist;
                    if (IntersectQuad(r.world, widthM, heightM, ctrl, u, v, dist)) {
                        hit = true; bestU = u; bestV = v; bestDist = dist; target = &pv;
                        break;
                    }
                }
                s_hover[hand].hit = hit;
                if (hit && target) { s_hover[hand].view = target->id; s_hover[hand].u = bestU; s_hover[hand].v = bestV; }

                // ── Grab-and-move ────────────────────────────────────
                // A page asked to be dragged: adopt the hand that is pointing
                // at it and remember how far away it was grabbed.
                if (const ViewId req = static_cast<ViewId>(s_dragRequest.load())) {
                    if (!s_dragView && hit && target && target->id == req) {
                        s_dragView = req;
                        s_dragHand = hand;
                        s_dragDist = std::clamp(bestDist, 0.35f, 6.0f);
                        s_dragRequest.store(0);
                        SKSE::log::info("Magelight VR: view {} grabbed by hand {} at {:.2f}m", req, hand, s_dragDist);
                    }
                }
                if (s_dragView && s_dragHand == hand) {
                    auto dit = s_overlays.find(s_dragView);
                    if (dit == s_overlays.end() || !dit->second.shown) {
                        s_dragView = 0; s_dragHand = -1;
                    } else {
                        OverlayRec& dr = dit->second;
                        // Hang the panel on the ray at the grab distance, then
                        // face it back at the head so it stays readable.
                        const float ox = ctrl.m[0][3], oy = ctrl.m[1][3], oz = ctrl.m[2][3];
                        const float dx = -ctrl.m[0][2], dy = -ctrl.m[1][2], dz = -ctrl.m[2][2];
                        dr.pos[0] = ox + dx * s_dragDist;
                        dr.pos[1] = oy + dy * s_dragDist;
                        dr.pos[2] = oz + dz * s_dragDist;
                        float fx = dr.pos[0] - hmd.mDeviceToAbsoluteTracking.m[0][3];
                        float fz = dr.pos[2] - hmd.mDeviceToAbsoluteTracking.m[2][3];
                        const float fn = std::sqrt(fx * fx + fz * fz);
                        if (fn > 1e-3f) { dr.fwd[0] = fx / fn; dr.fwd[1] = fz / fn; }
                        dr.placement.mode = Mode::WorldLocked;   // stays where you put it
                        dr.placed = true;
                        dr.following = false;
                        dr.world = LevelMatrix(dr.fwd, dr.pos);
                        if (s_overlay && s_compositor && dr.handle != vr::k_ulOverlayHandleInvalid)
                            s_overlay->SetOverlayTransformAbsolute(dr.handle, s_compositor->GetTrackingSpace(), &dr.world);
                    }
                }
                // Only draw the beam while it is ON one of our panels — the
                // runtime's own pointer behaves the same way (it appears near
                // its keyboard and nowhere else), and a beam sweeping the world
                // while you look around is just clutter.
                if (hit) UpdateBeam(hand, ctrl, hmd.mDeviceToAbsoluteTracking, bestDist);
                else HideBeam(hand);

                HandInput& hi = s_hands[hand];
                const bool nowDown = polled ? trig : hi.triggerDown.load();   // level (polled preferred)
                if (hit && target) {
                    const int px = static_cast<int>(std::lround(target->x + bestU * target->w));
                    const int py = static_cast<int>(std::lround(target->y + bestV * target->h));
                    // Only when it actually moved: each queued move costs a
                    // hitTest under the views mutex plus a WebKit hover
                    // hit-test on the present thread, and a steady hand
                    // otherwise pays that 90-180 times a second for nothing.
                    const bool moved = (px != s_lastPx[hand] || py != s_lastPy[hand]);
                    s_lastPx[hand] = px; s_lastPy[hand] = py;
                    const auto pos = static_cast<std::intptr_t>(MAKELPARAM(static_cast<WORD>(px), static_cast<WORD>(py)));
                    if (moved || (nowDown != s_lastTriggerDown[hand]))
                        Magelight::QueueSyntheticInput(WM_MOUSEMOVE, nowDown ? MK_LBUTTON : 0, pos);
                    if (nowDown && !s_lastTriggerDown[hand])
                        Magelight::QueueSyntheticInput(WM_LBUTTONDOWN, MK_LBUTTON, pos);
                }
                // Release at the LAST HIT pixel of this hand (a web click needs
                // the press and the release on the same element — 0.17.9 sent
                // the up at (0,0), which the host re-hit-tested to a corner far
                // outside every control). Fires whether or not the ray still
                // hits, so a drag that left the panel still releases.
                if (!nowDown && s_lastTriggerDown[hand]) {
                    const auto upPos = static_cast<std::intptr_t>(MAKELPARAM(static_cast<WORD>(s_lastPx[hand]), static_cast<WORD>(s_lastPy[hand])));
                    Magelight::QueueSyntheticInput(WM_LBUTTONUP, 0, upPos);
                }
                s_lastTriggerDown[hand] = nowDown;

                // Thumbstick Y -> wheel notches at the hit pixel (only while
                // pointing at a panel). Deadzone, rate-limited via an
                // accumulator, and a staleness guard so a stick that stopped
                // reporting never scrolls forever.
                {
                    float y;
                    if (polled) y = std::clamp(stickY, -1.0f, 1.0f);
                    else {
                        int mil = hi.stickY_mil.load();
                        if (GetTickCount64() - hi.stickTick.load() > kStickStaleMs) mil = 0;
                        y = mil / 1000.0f;
                    }
                    if (std::fabs(y) < kStickDeadzone) { y = 0.0f; s_scrollAccum[hand] = 0.0f; }
                    else y = (y > 0 ? 1.0f : -1.0f) * (std::fabs(y) - kStickDeadzone) / (1.0f - kStickDeadzone);
                    if (y != 0.0f && hit && target) {
                        s_scrollAccum[hand] += y * kScrollNotchesPerSec * std::clamp(s_frameDt, 0.0f, 0.1f);
                        const int px = static_cast<int>(std::lround(target->x + bestU * target->w));
                        const int py = static_cast<int>(std::lround(target->y + bestV * target->h));
                        const auto pos = static_cast<std::intptr_t>(MAKELPARAM(static_cast<WORD>(px), static_cast<WORD>(py)));
                        while (std::fabs(s_scrollAccum[hand]) >= 1.0f) {
                            const short delta = static_cast<short>(s_scrollAccum[hand] > 0 ? WHEEL_DELTA : -WHEEL_DELTA);   // stick up = wheel up
                            Magelight::QueueSyntheticInput(WM_MOUSEWHEEL,
                                static_cast<std::uintptr_t>(MAKEWPARAM(0, static_cast<WORD>(delta))), pos);
                            s_scrollAccum[hand] += (delta > 0) ? -1.0f : 1.0f;
                        }
                    }
                }
            }
        }
    }

    // ── Lifecycle ───────────────────────────────────────────────────────────
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

    // VR-1 probe: one overlay, created hidden, never shown, destroyed in
    // Shutdown. Its two log lines are the milestone: the runtime accepts our
    // overlay creation on this thread and releases it cleanly.
    static void CreateProbeOverlay()
    {
        if (!s_overlay) return;
        const auto err = s_overlay->CreateOverlay("severause.magelight.probe", "Magelight probe", &s_probe);
        if (err != vr::VROverlayError_None) {
            SKSE::log::error("Magelight VR: probe CreateOverlay failed ({}) — view overlays would fail the same way",
                static_cast<int>(err));
            s_probe = vr::k_ulOverlayHandleInvalid;
            return;
        }
        s_overlay->SetOverlayWidthInMeters(s_probe, 0.5f);
        SKSE::log::info("Magelight VR: probe overlay created (handle {}) — hidden by design", s_probe);
    }

    void Init(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        s_isVR = REL::Module::IsVR();
        s_thread = GetCurrentThreadId();
        s_device = device;
        s_context = context;
        if (!s_isVR) { SetState(State::Dormant); return; }
        if (!s_settings.enabled) {
            SKSE::log::info("Magelight VR: disabled by Magelight.json — presenter dormant");
            SetState(State::Dormant);
            return;
        }
        SKSE::log::info("Magelight VR: Skyrim VR runtime detected (init on thread {}; submitViews={}, mirror={}, alpha={})",
            s_thread, s_settings.submitViews, s_settings.mirror, s_settings.straightAlpha ? "straight" : "premultiplied");
        if (!BindInterfaces()) { SetState(State::Failed); return; }
        SetState(State::Live);
        CreateProbeOverlay();
        // Diagnostic: which tracked devices the runtime reports right now.
        if (s_system) {
            int devices = 0;
            for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i)
                if (s_system->IsTrackedDeviceConnected(i)) ++devices;
            SKSE::log::info("Magelight VR: {} tracked devices connected; hmd present={}", devices,
                s_system->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd));
        }
    }

    bool AnyButtonHeld()
    {
        if (GetState() != State::Live || !s_system) return false;
        for (int hand = 0; hand < 2; ++hand) {
            const auto role = (hand == 1) ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand;
            const vr::TrackedDeviceIndex_t idx = s_system->GetTrackedDeviceIndexForControllerRole(role);
            if (idx == vr::k_unTrackedDeviceIndexInvalid || idx >= vr::k_unMaxTrackedDeviceCount) continue;
            vr::VRControllerState_t st{};
            if (!s_system->GetControllerState(idx, &st, sizeof(st))) continue;
            for (const std::uint32_t code : kEdgeCodes)
                if (ButtonHeld(st, code)) return true;
        }
        return false;
    }

    void SubmitFrame(const PresentedFrame& frame, bool uiModeOn, ViewId /*uiModeView*/)
    {
        // Frame time for the follow glide + scroll rate (present thread).
        {
            const auto now = std::chrono::steady_clock::now();
            s_frameDt = (s_lastFrameTp.time_since_epoch().count() == 0) ? 0.0f
                      : std::chrono::duration<float>(now - s_lastFrameTp).count();
            s_lastFrameTp = now;
        }
        // The runtime-restart check has to run BEFORE the Live gate, or a
        // single transient bind failure is terminal for the session: the only
        // exit from Failed is the rebind below, and VR::Init runs once per
        // process. Checked first, a restart recovers us either way.
        if (s_getInitToken && s_getInitToken() != s_initToken && GetState() != State::Live) {
            SKSE::log::warn("Magelight VR: OpenVR init token changed while not live — rebinding");
            {
                std::lock_guard<std::mutex> lk(s_placementMutex);
                for (auto& [id, r] : s_overlays) { ReleaseRecLocked(r); r.handle = vr::k_ulOverlayHandleInvalid; }
            }
            ForgetBeams();
            s_aim[0] = AimCache{}; s_aim[1] = AimCache{};
            ++s_bindGeneration;
            SetState(BindInterfaces() ? State::Live : State::Failed);
            return;
        }
        if (GetState() != State::Live) return;
        if (GetCurrentThreadId() != s_thread) {   // invariant 1, mirrored — and a VR-1 telemetry line
            static bool warned = false;
            if (!warned) {
                warned = true;
                SKSE::log::warn("Magelight VR: SubmitFrame on thread {} (init thread {}) — skipping VR work on this thread",
                    GetCurrentThreadId(), s_thread);
            }
            return;
        }
        if (!s_settings.submitViews) return;     // VR-1: probe only; VR-2 flips this on

        // Runtime restart: every handle is dead. Rebind, next frame recreates.
        if (s_getInitToken && s_getInitToken() != s_initToken) {
            SKSE::log::warn("Magelight VR: OpenVR init token changed — rebinding");
            // ReleaseRecLocked guards every runtime teardown call on
            // `handle != invalid`, so nulling FIRST skipped HideOverlay /
            // ClearOverlayTexture / DestroyOverlay entirely and orphaned the
            // overlay inside the runtime under its stable key. The next
            // CreateOverlay for that view then failed KeyInUse forever.
            for (auto& [id, r] : s_overlays) { ReleaseRecLocked(r); r.handle = vr::k_ulOverlayHandleInvalid; }
            ForgetBeams();
            s_aim[0] = AimCache{}; s_aim[1] = AimCache{};
            ++s_bindGeneration;
            SetState(BindInterfaces() ? State::Live : State::Failed);
            return;
        }

        // One pose snapshot per frame (M1): the compositor's RENDER poses in
        // the space the overlays are composited in. Shared by the follow glide
        // and the laser.
        s_frameHmdUsable = false;
        {
            vr::TrackedDevicePose_t game[vr::k_unMaxTrackedDeviceCount];
            if (s_compositor && s_compositor->GetLastPoses(s_frameRender, vr::k_unMaxTrackedDeviceCount,
                                                           game, vr::k_unMaxTrackedDeviceCount) == vr::VRCompositorError_None)
                s_frameHmdUsable = PoseUsable(s_frameRender[vr::k_unTrackedDeviceIndex_Hmd]);
        }

        // Recenter requests (placement overrides are merged per view below,
        // AFTER the layer default is seeded — applying them here would create
        // the registry entry early and lose that default).
        {
            std::lock_guard<std::mutex> lk(s_placementMutex);
            for (auto& [id, _] : s_recenterRequests) {
                auto it = s_overlays.find(id);
                if (it != s_overlays.end()) { it->second.placed = false; it->second.following = false; }
            }
            s_recenterRequests.clear();
        }

        // Present every visible, non-inspector view; hide the rest.
        for (const PresentedView& pv : frame) {
            if (pv.isInspector || !pv.ul || !pv.srv || pv.w <= 0 || pv.h <= 0) continue;
            const bool fresh = s_overlays.find(pv.id) == s_overlays.end();
            OverlayRec& r = s_overlays[pv.id];
            if (fresh) r.placement = DefaultPlacementForLayer(pv.layer);
            // A queued override MERGES onto that default: 0 means "keep the
            // host's value for this layer" (a manifest that only names a mode
            // must not drag a HUD badge to the panel distance — field
            // 2026-09-03: the badge came out 1.40m at 1.60m instead of
            // 0.45m at 1.50m).
            {
                std::lock_guard<std::mutex> lk(s_placementMutex);
                if (s_placementResetRequests.erase(pv.id) > 0) {
                    // SetViewVRPlacement(view, nullptr): restore the LAYER
                    // default rather than storing a concrete default that
                    // clobbers it (review 2026-09-05, finding 6).
                    r.placement = DefaultPlacementForLayer(pv.layer);
                    r.placed = false; r.following = false;
                    s_placementOverrides.erase(pv.id);
                    if (r.handle != vr::k_ulOverlayHandleInvalid)
                        s_overlay->SetOverlayWidthInMeters(r.handle, ClampWidthM(r.placement.widthMeters));
                }
                else if (auto ov = s_placementOverrides.find(pv.id); ov != s_placementOverrides.end()) {
                    const Placement& p = ov->second;
                    r.placement.mode = p.mode;
                    if (p.distanceMeters > 0.0f) r.placement.distanceMeters = p.distanceMeters;
                    if (p.widthMeters > 0.0f)    r.placement.widthMeters    = p.widthMeters;
                    r.placement.heightOffset = p.heightOffset;
                    r.placed = false; r.following = false;   // re-place with the new values
                    s_placementOverrides.erase(ov);
                    if (r.handle != vr::k_ulOverlayHandleInvalid)
                        s_overlay->SetOverlayWidthInMeters(r.handle, ClampWidthM(r.placement.widthMeters));
                }
                s_placementSnapshot[pv.id] = r.placement;   // finding 3: publish under the lock
            }
            if (!EnsureOverlay(pv.id, r, pv.layer)) continue;
            if (r.placement.mode == Mode::HeadLocked) {
                // Glued: world = HMD * relative, pushed as an ABSOLUTE transform
                // every frame the head pose is usable. OpenComposite put the
                // TrackedDeviceRelative badge on the floor (field 2026-09-03);
                // the absolute path is what both runtimes composite correctly,
                // and it is the same matrix the laser hits. One frame of lag on
                // a HUD widget is accepted. Runtime-relative remains the
                // first-placement fallback while no head pose is available.
                if (s_frameHmdUsable && s_overlay && s_compositor) {
                    r.world = MatMul(s_frameRender[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking,
                                     HeadRelative(r.placement));
                    s_overlay->SetOverlayTransformAbsolute(r.handle, s_compositor->GetTrackingSpace(), &r.world);
                    r.placed = true;
                } else if (!r.placed) ApplyPlacement(r, r.placement);
            } else if (s_frameHmdUsable) {
                TickFollow(r, s_frameRender[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking, s_frameDt);
            }
            // A panel whose transform was never authored sits at the
            // tracking-space ORIGIN — on the floor, usually behind the player,
            // while the desktop mirror renders it perfectly. That is the
            // "open on the monitor, invisible in the headset" report. It
            // happens whenever the head pose was unusable on the frame the
            // overlay was created, because TickFollow is the only thing that
            // writes r.world for a non-HeadLocked view. Wait for a pose.
            if (r.placement.mode != Mode::HeadLocked && !r.placed) continue;
            if (!EnsureTexture(r, pv.w, pv.h)) continue;

            // The copy pass (design §2): the runtime only ever sees r.tex.
            // Cursor marks: last frame's laser hits on THIS view (per hand).
            CursorMark marks[2]; int nm = 0;
            if (uiModeOn)
                for (int h = 0; h < 2; ++h)
                    if (s_hover[h].hit && s_hover[h].view == pv.id) { marks[nm].u = s_hover[h].u; marks[nm].v = s_hover[h].v; ++nm; }
            // ── Submit only what changed ──────────────────────────────
            // This used to run EVERY frame regardless: a full-panel render-
            // target copy plus SetOverlayTexture, which hands a texture
            // across to the compositor process. At 1600x900 that is ~5.8 MB
            // per frame, ~90 times a second, for a page that is usually
            // perfectly still. The flat compositor has no equivalent cost
            // (it draws the SRV straight into a backbuffer it is already
            // writing), so this overlay-submission cost stayed invisible
            // until a page was driven from inside a headset.
            //
            // A view is resubmitted when its pixels changed (needs_paint,
            // sampled before Render cleared it), when the laser dot moved
            // (we stamp that INTO the texture), or when there is no valid
            // submission yet. Everything else the compositor already holds.
            // Quantise to a texel BEFORE comparing, and stamp the quantised
            // value, so the mark the gate tests is the mark that gets drawn.
            // Raw u/v come straight from the ray-quad intersection, so hand
            // tremor of a fraction of a degree moved them every single frame —
            // which made marksMoved true whenever the laser touched a panel
            // and defeated the whole needs_paint gate exactly when the user
            // was interacting.
            for (int i = 0; i < nm; ++i) {
                if (r.texW > 0) marks[i].u = std::lround(marks[i].u * r.texW) / static_cast<float>(r.texW);
                if (r.texH > 0) marks[i].v = std::lround(marks[i].v * r.texH) / static_cast<float>(r.texH);
            }
            bool marksMoved = (nm != r.lastMarkCount);
            for (int i = 0; !marksMoved && i < nm; ++i)
                marksMoved = (marks[i].u != r.lastMarkU[i]) || (marks[i].v != r.lastMarkV[i]);
            const bool needSubmit = pv.repainted || marksMoved || !r.everSubmitted || !r.shown;
            if (!needSubmit) continue;
            ++s_vrSubmits;
            r.lastMarkCount = nm;
            for (int i = 0; i < 2; ++i) {
                r.lastMarkU[i] = (i < nm) ? marks[i].u : -1.0f;
                r.lastMarkV[i] = (i < nm) ? marks[i].v : -1.0f;
            }

            Magelight::CopyViewToTarget(pv, r.rtv, pv.w, pv.h, s_settings.straightAlpha, nm ? marks : nullptr, nm);

            vr::Texture_t tex{ r.tex, vr::TextureType_DirectX, vr::ColorSpace_Gamma };
            const auto err = s_overlay->SetOverlayTexture(r.handle, &tex);
            if (err == vr::VROverlayError_None) r.everSubmitted = true;
            if (err != vr::VROverlayError_None) {
                static int s_logged = 0;
                if (s_logged++ < 4)
                    SKSE::log::error("Magelight VR: SetOverlayTexture view {} failed ({}) — PermissionDenied means the "
                                     "proxied interface, see design §7", pv.id, static_cast<int>(err));
                continue;
            }
            if (!r.shown) {
                const auto serr = s_overlay->ShowOverlay(r.handle);
                // Only latch on success: !r.shown is one of the terms that
                // forces a resubmit, so latching a FAILED show meant the view
                // was never retried and stayed invisible for good.
                r.shown = (serr == vr::VROverlayError_None);
                SKSE::log::info("Magelight VR: overlay shown for view {} ({}x{}, {})", pv.id, pv.w, pv.h, static_cast<int>(serr));
            }
        }
        ++s_vrFrames;   // FRAMES, not view-frames — the denominator must be comparable
        if (s_vrFrames - s_vrReported >= 600) {
            SKSE::log::info("Magelight VR: submit rate — {} of the last {} presented frames "
                            "needed a texture upload ({}%)",
                            s_vrSubmits, s_vrFrames,
                            s_vrFrames ? (s_vrSubmits * 100 / s_vrFrames) : 0);
            s_vrReported = s_vrFrames;
            s_vrSubmits = 0;
            s_vrFrames = 0;
            s_vrReported = 0;
        }

        for (auto& [id, r] : s_overlays) {
            bool present = false;
            for (const PresentedView& pv : frame) if (pv.id == id && !pv.isInspector && pv.ul) { present = true; break; }
            if (!present && r.shown && r.handle != vr::k_ulOverlayHandleInvalid) {
                s_overlay->HideOverlay(r.handle);
                r.shown = false;
                // Forget the anchor so the next SHOW re-places the panel in
                // front of wherever the head is looking THEN. Without this a
                // WorldLocked panel keeps the transform it was first given
                // for the whole session: field 2026-09-03 opened the UI once
                // on load, played on for three minutes, and every later open
                // was invisible in the headset while the desktop mirror
                // showed it perfectly — the quad was still sitting where the
                // player had been standing when they first opened it. Lazy
                // follow used to hide this by continuously re-aiming, so it
                // only surfaced once follow became opt-in (0.22.0). Placed
                // and STAYS placed is the contract, but the placement is per
                // opening, not per session.
                r.placed = false;
                r.following = false;
                SKSE::log::info("Magelight VR: overlay hidden for view {}", id);
            }
        }

        TickVRHotkeys();          // polled with UI mode OFF too — that is how a view opens
        if (uiModeOn) { TickLaser(frame); TickKeyboard(); }
        else ClearLaserState();   // beams off, hover forgotten, click levels reset
    }

    void ReleaseView(ViewId view, bool hibernating)
    {
        if (!hibernating) UnbindView(view);   // a hibernated view keeps its chord (see the header)
        {
            std::lock_guard<std::mutex> lk(s_placementMutex);
            s_placementSnapshot.erase(view);
            s_placementResetRequests.erase(view);
        }
        auto it = s_overlays.find(view);
        if (it == s_overlays.end()) return;
        ReleaseRecLocked(it->second);
        s_overlays.erase(it);
        SKSE::log::info("Magelight VR: overlay released for view {}", view);
    }

    void Shutdown()
    {
        for (auto& [id, r] : s_overlays) ReleaseRecLocked(r);
        s_overlays.clear();
        ReleaseBeams();
        if (s_overlay && s_probe != vr::k_ulOverlayHandleInvalid) {
            const auto err = s_overlay->DestroyOverlay(s_probe);
            SKSE::log::info("Magelight VR: probe overlay destroyed ({})", static_cast<int>(err));
            s_probe = vr::k_ulOverlayHandleInvalid;
        }
        s_system = nullptr; s_compositor = nullptr; s_overlay = nullptr;
        if (GetState() == State::Live) SetState(State::Dormant);
    }

    // ── Queries / placement API ─────────────────────────────────────────────
    State GetState() { return static_cast<State>(s_state.load()); }
    bool  IsVRRuntime() { return s_isVR; }

    void SetPlacement(ViewId view, const Placement& p)
    {
        std::lock_guard<std::mutex> lk(s_placementMutex);
        s_placementOverrides[view] = p;
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
        // through the capture must not fire a view when capture ends (finding 1).
        ++s_bindEpoch;
        // Ask the present-thread tick to seed the capture masks ("suppress
        // everything currently held, so the button the user clicked Rebind
        // with is not read as a fresh edge"). Writing that seed HERE, on the
        // game thread, raced the tick's own mask writes (finding 7).
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

    void NoteTextFocus(ViewId view, bool focused)
    {
        // Render thread (the JS dispatcher). TickKeyboard acts on it next frame.
        if (focused) s_kbView = view;
        else if (s_kbView == view) s_kbView = 0;
    }

    bool GetPlacement(ViewId view, Placement& out)
    {
        // Read the snapshot the present thread publishes each frame under this
        // lock — never a lock-free s_overlays.find() from the game thread while
        // the present thread inserts/erases that map (review 2026-09-05, finding 3).
        std::lock_guard<std::mutex> lk(s_placementMutex);
        auto it = s_placementSnapshot.find(view);
        if (it == s_placementSnapshot.end()) return false;
        out = it->second;
        return true;
    }

    void ClearPlacement(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_placementMutex);
        s_placementOverrides.erase(view);
        s_placementResetRequests[view] = true;
    }

    void Recenter(ViewId view)
    {
        std::lock_guard<std::mutex> lk(s_placementMutex);
        s_recenterRequests[view] = true;
    }

    void BeginViewDrag(ViewId view) { s_dragRequest.store(view); }
    void EndViewDrag()
    {
        if (s_dragView) SKSE::log::info("Magelight VR: view {} released", s_dragView);
        s_dragRequest.store(0);
        s_dragView = 0;
        s_dragHand = -1;
    }

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

// ── Still to wire (VR-3/VR-4) ───────────────────────────────────────────────
//  InputSink             for kButton events whose device is a VR controller:
//                        VR::NoteButton(device, code, down) — pass-through
//                        otherwise unchanged; thumbstick events -> NoteThumbstick
//  SetUIModeImpl         VR branch: no CursorMenu / MenuCursor hide when
//                        VR::IsLive() (verify whether the CursorMenu is even
//                        drivable under SkyrimVR)
//  MagelightApi4         the v4 tail: SetViewVRPlacement / GetViewVRPlacement /
//                        RecenterVRView
