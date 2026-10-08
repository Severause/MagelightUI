// Magelight UI — Skyrim VR presenter: the per-view overlay records — create,
// texture, place, follow, release (docs/VR_PRESENTER.md).
// Shared state and its rules: State.h.

#include "State.h"

namespace Magelight::VR {

    // ── Per-view overlay records (Ultralight thread only) ───────────────────
    std::map<ViewId, OverlayRec> s_overlays;

    // Bumped on every (re)bind of the OpenVR interfaces. Part of each
    // overlay key so a rebind can never collide with an overlay orphaned
    // inside the runtime by a restart mid-teardown.
    std::uint64_t s_bindGeneration = 0;

    namespace {
        const char* kOverlayKeyPrefix = "severause.magelight.view.";
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
        // No MISC_SHARED: SteamVR's vrclient copies in-process. Set it if a
        // runtime refuses the texture.
        if (FAILED(s_device->CreateTexture2D(&td, nullptr, &r.tex))) return false;
        if (FAILED(s_device->CreateRenderTargetView(r.tex, nullptr, &r.rtv))) return false;
        if (FAILED(s_device->CreateShaderResourceView(r.tex, nullptr, &r.srv))) return false;
        // Transparent until the copy pass fills it: a fresh D3D
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

    // HeadLocked: the runtime-relative transform, SubmitFrame's first
    // placement while no head pose is usable (with one, SubmitFrame authors
    // the absolute world itself). LazyFollow and WorldLocked are only
    // re-armed here: TickFollow places them from the live head pose.
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

    namespace {
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

}  // namespace Magelight::VR
