// Magelight UI — Skyrim VR presenter: Init and the probe overlay, the per-frame
// SubmitFrame, ReleaseView, Shutdown, and the placement API with the requests it
// queues (docs/VR_PRESENTER.md). Shared state and its rules: State.h.

#include "State.h"

namespace Magelight::VR {

    namespace {
        DWORD             s_thread = 0;            // the thread Init ran on (== Ultralight thread)
        vr::VROverlayHandle_t s_probe = vr::k_ulOverlayHandleInvalid;   // VR-1: the hidden probe overlay
        std::chrono::steady_clock::time_point s_lastFrameTp{};

        // Placement overrides from the API (any thread) — applied next frame.
        std::mutex s_placementMutex;
        std::map<ViewId, Placement> s_placementOverrides;
        std::map<ViewId, bool>      s_recenterRequests;
        // "reset to the layer default" requests (SetViewVRPlacement(view, nullptr)),
        // drained in SubmitFrame where the layer is known.
        std::map<ViewId, bool>      s_placementResetRequests;
        // A thread-safe COPY of each view's effective placement, published by
        // the present thread each frame under s_placementMutex; GetPlacement
        // reads THIS, never s_overlays (which the present thread mutates
        // lock-free).
        std::map<ViewId, Placement> s_placementSnapshot;

        // VR submit accounting. Logged periodically while a panel is up so a
        // field log SAYS whether the overlay path is doing per-frame work,
        // instead of us inferring it from symptoms.
        std::uint64_t s_vrFrames = 0, s_vrSubmits = 0, s_vrReported = 0;
    }

    // ── Lifecycle ───────────────────────────────────────────────────────────
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
        if (GetState() != State::Live) { s_runtimeHeldMask.store(0); return; }
        {
            const std::uint32_t mask = PollHeldMask();
            const std::uint32_t pressed = mask & ~s_runtimeHeldMask.exchange(mask);
            if (pressed) {
                const std::uint64_t now = GetTickCount64();
                for (int i = 0; i < 12; ++i)
                    if (pressed & (1u << i)) s_runtimePressMs[i].store(now);
            }
        }
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
            // must not drag a HUD badge to the panel distance).
            {
                std::lock_guard<std::mutex> lk(s_placementMutex);
                if (s_placementResetRequests.erase(pv.id) > 0) {
                    // SetViewVRPlacement(view, nullptr): restore the LAYER
                    // default rather than storing a concrete default that
                    // clobbers it.
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
                s_placementSnapshot[pv.id] = r.placement;   // published under the lock for GetPlacement
            }
            if (!EnsureOverlay(pv.id, r, pv.layer)) continue;
            if (r.placement.mode == Mode::HeadLocked) {
                // Glued: world = HMD * relative, pushed as an ABSOLUTE transform
                // every frame the head pose is usable. OpenComposite puts a
                // TrackedDeviceRelative HUD overlay on the floor; the absolute
                // path is what both runtimes composite correctly, and it is the
                // same matrix the laser hits. One frame of lag on a HUD widget
                // is accepted. Runtime-relative remains the first-placement
                // fallback while no head pose is available.
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
            // while the desktop mirror renders it perfectly: open on the
            // monitor, invisible in the headset. It happens whenever the head
            // pose was unusable on the frame the overlay was created, because
            // TickFollow is the only thing that writes r.world for a
            // non-HeadLocked view. Wait for a pose.
            if (r.placement.mode != Mode::HeadLocked && !r.placed) continue;
            if (!EnsureTexture(r, pv.w, pv.h)) continue;

            // The copy pass (design §2): the runtime only ever sees r.tex.
            // Cursor marks: last frame's laser hits on THIS view (per hand).
            CursorMark marks[2]; int nm = 0;
            if (uiModeOn)
                for (int h = 0; h < 2; ++h)
                    if (s_hover[h].hit && s_hover[h].view == pv.id) { marks[nm].u = s_hover[h].u; marks[nm].v = s_hover[h].v; ++nm; }
            // ── Submit only what changed ──────────────────────────────
            // A submission is a full-panel render-target copy plus
            // SetOverlayTexture, which hands a texture across to the
            // compositor process: at 1600x900 ~5.8 MB per frame, ~90 times a
            // second, for a page that is usually perfectly still. The flat
            // compositor has no equivalent cost (it draws the SRV straight
            // into a backbuffer it is already writing), so this one only
            // shows in a headset.
            //
            // A view is resubmitted when its pixels changed (needs_paint,
            // sampled before Render cleared it), when the laser dot moved
            // (we stamp that INTO the texture), or when there is no valid
            // submission yet. Everything else the compositor already holds.
            // Quantise to a texel BEFORE comparing, and stamp the quantised
            // value, so the mark the gate tests is the mark that gets drawn.
            // Raw u/v come straight from the ray-quad intersection, where hand
            // tremor of a fraction of a degree moves them every frame; left
            // unquantised, marksMoved would be true whenever the laser touches
            // a panel, defeating the needs_paint gate exactly when the user
            // interacts.
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
                // forces a resubmit, so latching a FAILED show would never
                // retry it and leave the view invisible for good.
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
                // for the whole session, and every later open is invisible
                // in the headset (the quad still sits where the player stood
                // when they first opened it) while the desktop mirror shows
                // it perfectly. Placed and STAYS placed is the contract, but
                // the placement is per opening, not per session.
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
        if (!hibernating) UnbindView(view);   // a hibernated view keeps its chord (see MagelightVR.h, ReleaseView)
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

    // ── Placement API ───────────────────────────────────────────────────────
    void SetPlacement(ViewId view, const Placement& p)
    {
        std::lock_guard<std::mutex> lk(s_placementMutex);
        s_placementOverrides[view] = p;
    }

    bool GetPlacement(ViewId view, Placement& out)
    {
        // Read the snapshot the present thread publishes each frame under this
        // lock — never a lock-free s_overlays.find() from the game thread while
        // the present thread inserts/erases that map.
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

}  // namespace Magelight::VR
