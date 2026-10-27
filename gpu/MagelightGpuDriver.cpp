// MagelightGPU — Direct3D 11 GPUDriver for Ultralight 2.0, adapted from the Ultralight 2.0.0-beta.2
// SDK's reference driver: platform/src/GPUDriver.cpp, platform/src/gpu/d3d11/GPUDriverD3D11.cpp and
// platform/src/gpu/d3d11/GPUContextD3D11.cpp (Zlib licence, reproduced below). This is an ALTERED
// version of that source: the changes are marked "MG:" (see gpu/README.md for the list).
//
// Copyright (c) 2026 Ultralight, Inc.
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would
//    be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source
//    distribution.
//
// The compiled shader bytecode this file embeds is NOT covered by that licence: it comes from the SDK's
// platform/shaders/generated/headers/d3d11 through the include path (never copied into this repository) and
// is proprietary to Ultralight, Inc., licensed only for use with the Ultralight library.

#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <Ultralight/Bitmap.h>
#include <Ultralight/Matrix.h>
#include <Ultralight/platform/GPUDriver.h>

#include "MagelightGpuApi.h"

// The SDK's generated DXBC (vertex_path/vertex_quad/vertex_photon_grid VS, fill/fill_path/fill_photon/
// fill_photon_grid/filter_basic/filter_blur/filter_dropshadow PS), found through the build's include path.
#include <d3d11/shaders.h>

using Microsoft::WRL::ComPtr;

namespace {

    // MG: the host's logger replaces the SDK's platform Logger and MessageBoxW. Every message is logged once per
    // driver lifetime (the SDK's LogErrorOnce): a failing Create*/Bind* recurs every repaint.
    MgGpuLogFn g_log = nullptr;
    std::mutex g_logMutex;
    std::set<std::string> g_logSeen;

    void LogErrorOnce(const char* msg)
    {
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            if (!g_logSeen.insert(msg).second) return;
        }
        if (g_log) g_log(msg);
    }

    // MG: "<what> failed (0x...)", plus the device-removed reason when the device is gone.
    void LogFailure(const char* what, HRESULT hr, ID3D11Device* device)
    {
        char line[192];
        const HRESULT removed = device ? device->GetDeviceRemovedReason() : S_OK;
        if (FAILED(removed))
            std::snprintf(line, sizeof(line), "MgGpu: %s failed (0x%08X) - the device was removed (0x%08X)", what,
                static_cast<unsigned>(hr), static_cast<unsigned>(removed));
        else
            std::snprintf(line, sizeof(line), "MgGpu: %s failed (0x%08X)", what, static_cast<unsigned>(hr));
        LogErrorOnce(line);
    }

    // CB0 of every program (50 registers in the pixel shaders, the first 5 in the vertex shaders).
    struct Uniforms {
        DirectX::XMFLOAT4 State;      // time, screenWidth, screenHeight, screenScale
        DirectX::XMMATRIX Transform;  // model-view-projection
        DirectX::XMINT4 Integer4[2];
        DirectX::XMFLOAT4 Scalar4[2];
        DirectX::XMFLOAT4 Vector[8];
        DirectX::XMINT4 ClipData;     // x = clip size
        DirectX::XMMATRIX Clip[8];
    };
    static_assert(sizeof(Uniforms) == 800, "Uniforms must match the shaders' 50-register CB0");

}  // namespace

namespace mggpu {

    using namespace ultralight;

    // ── Base driver (SDK platform/src/GPUDriver.cpp) ────────────────────────────
    // Id generation and command-list batching; DrawCommandList dispatches each command to the immediate-mode
    // primitives below.
    class GPUDriverImpl : public ultralight::GPUDriver {
    public:
        ~GPUDriverImpl() override {}

        virtual void BindTexture(uint8_t texture_unit, uint32_t texture_id) = 0;
        virtual void ClearRenderBuffer(uint32_t render_buffer_id) = 0;
        virtual void DrawGeometry(uint32_t geometry_id, uint32_t indices_count, uint32_t indices_offset,
                                  const GPUState& state) = 0;
        // CommandType::Flush: the engine is about to re-target a render buffer it sampled earlier in this list.
        virtual void Flush() {}

        bool HasCommandsPending() const { return !command_list_.empty(); }

        virtual void DrawCommandList()
        {
            if (command_list_.empty()) return;
            batch_count_ = 0;
            for (auto& cmd : command_list_) {
                if (cmd.command_type == CommandType::DrawGeometry)
                    DrawGeometry(cmd.geometry_id, cmd.indices_count, cmd.indices_offset, cmd.gpu_state);
                else if (cmd.command_type == CommandType::ClearRenderBuffer)
                    ClearRenderBuffer(cmd.gpu_state.render_buffer_id);
                else if (cmd.command_type == CommandType::Flush) {
                    Flush();
                    ++flushes_;  // MG: MgGpu_GetStats accounting
                }
                batch_count_++;
            }
            command_list_.clear();
        }

        int batch_count() const { return batch_count_; }
        std::uint64_t flushes() const { return flushes_; }  // MG: MgGpu_GetStats accounting

        void BeginSynchronize() override {}
        void EndSynchronize() override {}
        uint32_t NextTextureId() override { return next_texture_id_++; }
        uint32_t NextRenderBufferId() override { return next_render_buffer_id_++; }
        uint32_t NextGeometryId() override { return next_geometry_id_++; }

        void UpdateCommandList(const CommandList& list) override
        {
            if (list.size) {
                command_list_.resize(list.size);
                std::memcpy(command_list_.data(), list.commands, sizeof(Command) * list.size);
            }
        }

    protected:
        uint32_t next_texture_id_ = 1;
        uint32_t next_render_buffer_id_ = 1;  // 0 is reserved
        uint32_t next_geometry_id_ = 1;
        std::vector<Command> command_list_;
        int batch_count_ = 0;
        std::uint64_t flushes_ = 0;  // MG: MgGpu_GetStats accounting
    };

    // ── Blend mapping (SDK GPUContextD3D11.cpp) ─────────────────────────────────
    D3D11_BLEND MapBlendFactor(uint8_t factor)
    {
        switch (static_cast<BlendFactor>(factor)) {
        case BlendFactor::Zero:             return D3D11_BLEND_ZERO;
        case BlendFactor::One:              return D3D11_BLEND_ONE;
        case BlendFactor::SrcColor:         return D3D11_BLEND_SRC_COLOR;
        case BlendFactor::InvSrcColor:      return D3D11_BLEND_INV_SRC_COLOR;
        case BlendFactor::SrcAlpha:         return D3D11_BLEND_SRC_ALPHA;
        case BlendFactor::InvSrcAlpha:      return D3D11_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DestColor:        return D3D11_BLEND_DEST_COLOR;
        case BlendFactor::InvDestColor:     return D3D11_BLEND_INV_DEST_COLOR;
        case BlendFactor::DestAlpha:        return D3D11_BLEND_DEST_ALPHA;
        case BlendFactor::InvDestAlpha:     return D3D11_BLEND_INV_DEST_ALPHA;
        case BlendFactor::SrcAlphaSaturate: return D3D11_BLEND_SRC_ALPHA_SAT;
        default:                            return D3D11_BLEND_ONE;
        }
    }

    // D3D11 forbids *_COLOR factors on the alpha channel: substitute the matching alpha factor.
    D3D11_BLEND MapBlendFactorAlpha(uint8_t factor)
    {
        switch (static_cast<BlendFactor>(factor)) {
        case BlendFactor::SrcColor:     return D3D11_BLEND_SRC_ALPHA;
        case BlendFactor::InvSrcColor:  return D3D11_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DestColor:    return D3D11_BLEND_DEST_ALPHA;
        case BlendFactor::InvDestColor: return D3D11_BLEND_INV_DEST_ALPHA;
        default:                        return MapBlendFactor(factor);
        }
    }

    D3D11_BLEND_OP MapBlendEquation(uint8_t equation)
    {
        switch (static_cast<BlendEquation>(equation)) {
        case BlendEquation::Add:         return D3D11_BLEND_OP_ADD;
        case BlendEquation::Subtract:    return D3D11_BLEND_OP_SUBTRACT;
        case BlendEquation::RevSubtract: return D3D11_BLEND_OP_REV_SUBTRACT;
        case BlendEquation::Min:         return D3D11_BLEND_OP_MIN;
        case BlendEquation::Max:         return D3D11_BLEND_OP_MAX;
        default:                         return D3D11_BLEND_OP_ADD;
        }
    }

    // ── GPU context (SDK GPUContextD3D11, adapted) ──────────────────────────────
    // MG: wraps the GAME's device and immediate context (not owned) instead of creating a device; no swap chains;
    // creating it touches no pipeline state; the MSAA sample count is the host's (SetSampleCount), where the SDK
    // fixes 8x; the capabilities are measured on the game's device. The context may instead be a deferred context of
    // that device (the host's page thread): everything the driver does is then recorded, and the game's thread runs
    // the list at Present. immediate_context() names the context the driver draws through either way.
    class GPUContextD3D11 {
    public:
        GPUContextD3D11(ID3D11Device* device, ID3D11DeviceContext* context)
            : device_(device), immediate_context_(context)
        {
            immediate_context_->QueryInterface(IID_PPV_ARGS(context1_.GetAddressOf()));
            // MG: without command lists in the graphics driver the runtime emulates them, which reads a boxed
            // UpdateSubresource's source from another place (UpdateTexture).
            deferred_ = immediate_context_->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
            if (deferred_) {
                D3D11_FEATURE_DATA_THREADING threading{};
                native_lists_ = SUCCEEDED(device_->CheckFeatureSupport(D3D11_FEATURE_THREADING, &threading,
                                                                       sizeof(threading)))
                                && threading.DriverCommandLists;
            }
            feature_level_ = device_->GetFeatureLevel();
            uav_slots_ = (feature_level_ >= D3D_FEATURE_LEVEL_11_1 && context1_) ? D3D11_1_UAV_SLOT_COUNT
                                                                                 : D3D11_PS_CS_UAV_REGISTER_COUNT;

            D3D11_BLEND_DESC blend_desc;
            ZeroMemory(&blend_desc, sizeof(blend_desc));
            auto& rt = blend_desc.RenderTarget[0];
            rt.BlendEnable = false;
            rt.SrcBlend = D3D11_BLEND_ONE;
            rt.DestBlend = D3D11_BLEND_ZERO;
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_ZERO;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            const HRESULT hr = device_->CreateBlendState(&blend_desc, disabled_blend_state_.GetAddressOf());
            if (FAILED(hr)) LogFailure("CreateBlendState (disabled)", hr, device_);

            CreateRasterizerStates();
            MeasureCaps();
        }

        ID3D11Device* device() { return device_; }
        ID3D11DeviceContext* immediate_context() { return immediate_context_; }
        ID3D11DeviceContext1* context1() { return context1_.Get(); }
        bool deferred() const { return deferred_; }           // MG
        bool native_lists() const { return native_lists_; }   // MG: meaningful only when deferred()
        UINT uav_slots() const { return uav_slots_; }

        // MG: the MSAA sample count of antialiased render targets created from now on (1 = off). A count the
        // device cannot do for BGRA8 render targets steps down (8 -> 4 -> 2 -> 1). Once the engine has read the
        // capabilities (CapsReported), the count never rises above what it was told. Returns the count in effect.
        UINT SetSampleCount(UINT samples)
        {
            UINT n = samples >= 8 ? 8 : samples >= 4 ? 4 : samples >= 2 ? 2 : 1;
            if (caps_reported_ && n > caps_samples_) n = caps_samples_;
            while (n > 1) {
                UINT quality = 0;
                if (SUCCEEDED(device_->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, n, &quality))
                    && quality > 0)
                    break;
                n /= 2;
            }
            if (n != sample_count_) {
                sample_count_ = n;
                CreateRasterizerStates();
            }
            return sample_count_;
        }
        UINT sample_count() const { return sample_count_; }

        // MG: what the engine is told (GPUDriver::GetDeviceCaps), measured on the game's device rather than
        // assumed: a compressed family only when every format of it samples as a Texture2D.
        void FillCaps(GPUDeviceCaps& caps)
        {
            caps.compressed_formats = compressed_formats_;
            caps.max_texture_dimension = max_texture_dimension_;
            if (sample_count_ > 1) {
                caps.supports_msaa = 1;
                caps.max_msaa_samples = sample_count_;
            }
            // Render targets persist on a desktop D3D11 device, so a View redraws only what changed.
            caps.supports_partial_redraw = 1;
            caps_reported_ = true;
            caps_samples_ = sample_count_;
        }

        void SetBlendState(uint8_t src_factor, uint8_t dst_factor, uint8_t equation)
        {
            immediate_context_->OMSetBlendState(GetOrCreateBlendState(src_factor, dst_factor, equation), nullptr,
                                                0xffffffff);
        }
        void DisableBlend()
        {
            immediate_context_->OMSetBlendState(disabled_blend_state_.Get(), nullptr, 0xffffffff);
        }
        void EnableScissor() { immediate_context_->RSSetState(scissored_rasterizer_state_.Get()); }
        void DisableScissor() { immediate_context_->RSSetState(rasterizer_state_.Get()); }

    private:
        void CreateRasterizerStates()
        {
            rasterizer_state_.Reset();
            scissored_rasterizer_state_.Reset();
            D3D11_RASTERIZER_DESC desc;
            ZeroMemory(&desc, sizeof(desc));
            desc.FillMode = D3D11_FILL_SOLID;
            desc.CullMode = D3D11_CULL_NONE;
            desc.DepthClipEnable = false;
            desc.ScissorEnable = false;
            desc.MultisampleEnable = sample_count_ > 1;
            HRESULT hr = device_->CreateRasterizerState(&desc, rasterizer_state_.GetAddressOf());
            if (FAILED(hr)) LogFailure("CreateRasterizerState", hr, device_);
            desc.ScissorEnable = true;
            hr = device_->CreateRasterizerState(&desc, scissored_rasterizer_state_.GetAddressOf());
            if (FAILED(hr)) LogFailure("CreateRasterizerState (scissored)", hr, device_);
        }

        bool Samples2D(DXGI_FORMAT format) const
        {
            UINT support = 0;
            return SUCCEEDED(device_->CheckFormatSupport(format, &support))
                && (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) && (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);
        }

        void MeasureCaps()
        {
            compressed_formats_ = 0;
            if (Samples2D(DXGI_FORMAT_BC1_UNORM) && Samples2D(DXGI_FORMAT_BC2_UNORM) && Samples2D(DXGI_FORMAT_BC3_UNORM))
                compressed_formats_ |= kGPUCompressedFormat_BC1_BC3;
            if (Samples2D(DXGI_FORMAT_BC7_UNORM)) compressed_formats_ |= kGPUCompressedFormat_BC7;
            max_texture_dimension_ = feature_level_ >= D3D_FEATURE_LEVEL_11_0 ? 16384u
                : feature_level_ >= D3D_FEATURE_LEVEL_10_0                   ? 8192u
                : feature_level_ >= D3D_FEATURE_LEVEL_9_3                    ? 4096u
                                                                             : 2048u;
        }

        ID3D11BlendState* GetOrCreateBlendState(uint8_t src_factor, uint8_t dst_factor, uint8_t equation)
        {
            const uint32_t key = (static_cast<uint32_t>(src_factor) << 16) | (static_cast<uint32_t>(dst_factor) << 8)
                | static_cast<uint32_t>(equation);
            auto it = blend_state_cache_.find(key);
            if (it != blend_state_cache_.end()) return it->second.Get();

            D3D11_BLEND_DESC blend_desc;
            ZeroMemory(&blend_desc, sizeof(blend_desc));
            auto& rt = blend_desc.RenderTarget[0];
            rt.BlendEnable = true;
            rt.SrcBlend = MapBlendFactor(src_factor);
            rt.DestBlend = MapBlendFactor(dst_factor);
            rt.BlendOp = MapBlendEquation(equation);
            rt.SrcBlendAlpha = MapBlendFactorAlpha(src_factor);
            rt.DestBlendAlpha = MapBlendFactorAlpha(dst_factor);
            rt.BlendOpAlpha = rt.BlendOp;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            ComPtr<ID3D11BlendState> state;
            const HRESULT hr = device_->CreateBlendState(&blend_desc, state.GetAddressOf());
            if (FAILED(hr)) {
                // MG: the SDK falls back to a premultiplied-alpha state; caching null keeps the failure to one log
                // line, and a null blend state is the D3D11 default (blending off) rather than a stale game state.
                LogFailure("CreateBlendState", hr, device_);
            }
            blend_state_cache_[key] = state;
            return state.Get();
        }

        ID3D11Device* device_ = nullptr;                    // MG: not owned
        ID3D11DeviceContext* immediate_context_ = nullptr;  // MG: not owned
        ComPtr<ID3D11DeviceContext1> context1_;             // MG: null below the 11.1 runtime
        bool deferred_ = false;                             // MG: immediate_context_ is a deferred context
        bool native_lists_ = false;                         // MG: the graphics driver runs command lists itself
        D3D_FEATURE_LEVEL feature_level_ = D3D_FEATURE_LEVEL_11_0;
        UINT uav_slots_ = D3D11_PS_CS_UAV_REGISTER_COUNT;
        UINT sample_count_ = 1;
        bool caps_reported_ = false;
        UINT caps_samples_ = 1;
        uint32_t compressed_formats_ = 0;
        uint32_t max_texture_dimension_ = 16384;
        ComPtr<ID3D11BlendState> disabled_blend_state_;
        ComPtr<ID3D11RasterizerState> rasterizer_state_;
        ComPtr<ID3D11RasterizerState> scissored_rasterizer_state_;
        std::unordered_map<uint32_t, ComPtr<ID3D11BlendState>> blend_state_cache_;
    };

    // ── MG: the game's pipeline state around a command list ─────────────────────
    // The driver draws inside the game's frame on the game's immediate context, so DrawCommandList captures every
    // binding a draw can change, neutralizes the stages it does not bind (a game HS/DS or GS would void or bend
    // our draws, a predicate could skip them), and restores all of it afterwards. Covered: IA (topology, layout,
    // VB 0, IB), VS/PS (shader + class instances, CB0 with its 11.1 offsets, SRVs t0-t3), PS sampler s0, GS/HS/DS
    // shaders, OM (blend state/factor/mask, depth-stencil state/ref, all 8 RTVs + DSV and every UAV the 1-target
    // OMSetRenderTargets unbinds), RS (state, every viewport and scissor rect), SO targets and predication.
    // Render thread only; not re-entrant.
    class PipelineBackup {
    public:
        static constexpr UINT kSrvSlots = 4;  // the programs read t0-t3
        static constexpr UINT kVpSlots = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

        PipelineBackup()
        {
            for (auto& s : stages_) s.instances.resize(D3D11_SHADER_MAX_INTERFACES);
        }
        ~PipelineBackup() { ReleaseAll(); }

        void Capture(GPUContextD3D11& gctx)
        {
            ReleaseAll();
            ID3D11DeviceContext* c = gctx.immediate_context();
            ID3D11DeviceContext1* c1 = gctx.context1();
            uav_slots_ = gctx.uav_slots();

            c->IAGetPrimitiveTopology(&topo_);
            c->IAGetInputLayout(&layout_);
            c->IAGetVertexBuffers(0, 1, &vb_, &vb_stride_, &vb_offset_);
            c->IAGetIndexBuffer(&ib_, &ib_format_, &ib_offset_);

            GetStage(kVS, [&](UINT* n, ID3D11ClassInstance** ci) { c->VSGetShader(&vs_, ci, n); });
            GetStage(kPS, [&](UINT* n, ID3D11ClassInstance** ci) { c->PSGetShader(&ps_, ci, n); });
            GetStage(kGS, [&](UINT* n, ID3D11ClassInstance** ci) { c->GSGetShader(&gs_, ci, n); });
            GetStage(kHS, [&](UINT* n, ID3D11ClassInstance** ci) { c->HSGetShader(&hs_, ci, n); });
            GetStage(kDS, [&](UINT* n, ID3D11ClassInstance** ci) { c->DSGetShader(&ds_, ci, n); });

            c->VSGetShaderResources(0, kSrvSlots, vs_srvs_);
            c->PSGetShaderResources(0, kSrvSlots, ps_srvs_);
            c->PSGetSamplers(0, 1, &ps_sampler_);
            if (c1) {
                c1->VSGetConstantBuffers1(0, 1, &vs_cb_, &vs_cb_first_, &vs_cb_num_);
                c1->PSGetConstantBuffers1(0, 1, &ps_cb_, &ps_cb_first_, &ps_cb_num_);
            } else {
                c->VSGetConstantBuffers(0, 1, &vs_cb_);
                c->PSGetConstantBuffers(0, 1, &ps_cb_);
            }

            c->OMGetBlendState(&blend_, blend_factor_, &sample_mask_);
            c->OMGetDepthStencilState(&depth_, &stencil_ref_);
            c->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs_, &dsv_, 0,
                                                         uav_slots_, uavs_);

            c->RSGetState(&rs_);
            vp_count_ = kVpSlots;
            c->RSGetViewports(&vp_count_, vps_);
            sc_count_ = kVpSlots;
            c->RSGetScissorRects(&sc_count_, scs_);

            c->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so_);
            c->GetPredication(&predicate_, &predicate_value_);
            captured_ = true;
        }

        // The stages the driver never binds, set to what the SDK driver takes for granted on its own context.
        static void Neutralize(ID3D11DeviceContext* c)
        {
            c->GSSetShader(nullptr, nullptr, 0);
            c->HSSetShader(nullptr, nullptr, 0);
            c->DSSetShader(nullptr, nullptr, 0);
            c->OMSetDepthStencilState(nullptr, 0);
            ID3D11Buffer* no_so[D3D11_SO_BUFFER_SLOT_COUNT] = {};
            UINT so_offsets[D3D11_SO_BUFFER_SLOT_COUNT] = {};
            c->SOSetTargets(D3D11_SO_BUFFER_SLOT_COUNT, no_so, so_offsets);
            c->SetPredication(nullptr, FALSE);
        }

        void Restore(GPUContextD3D11& gctx)
        {
            if (!captured_) return;
            ID3D11DeviceContext* c = gctx.immediate_context();
            ID3D11DeviceContext1* c1 = gctx.context1();

            // Output merger first: re-binding the game's targets before its SRVs means no restored SRV is ever
            // unbound by a target bind (the runtime's read/write hazard rule).
            UINT rt_count = 0, uav_end = 0;
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
                if (rtvs_[i]) rt_count = i + 1;
            for (UINT i = 0; i < uav_slots_; ++i)
                if (uavs_[i]) uav_end = i + 1;
            if (uav_end > rt_count) {
                // A UAV sits in an OM slot past the last target; -1 keeps the append/consume counters.
                UINT keep[D3D11_1_UAV_SLOT_COUNT];
                for (auto& k : keep) k = static_cast<UINT>(-1);
                c->OMSetRenderTargetsAndUnorderedAccessViews(rt_count, rtvs_, dsv_, rt_count, uav_end - rt_count,
                                                             uavs_ + rt_count, keep);
            } else {
                c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs_, dsv_);
            }
            c->OMSetBlendState(blend_, blend_factor_, sample_mask_);
            c->OMSetDepthStencilState(depth_, stencil_ref_);

            c->IASetPrimitiveTopology(topo_);
            c->IASetInputLayout(layout_);
            c->IASetVertexBuffers(0, 1, &vb_, &vb_stride_, &vb_offset_);
            c->IASetIndexBuffer(ib_, ib_format_, ib_offset_);

            c->VSSetShader(vs_, Instances(kVS), stages_[kVS].count);
            c->PSSetShader(ps_, Instances(kPS), stages_[kPS].count);
            c->GSSetShader(gs_, Instances(kGS), stages_[kGS].count);
            c->HSSetShader(hs_, Instances(kHS), stages_[kHS].count);
            c->DSSetShader(ds_, Instances(kDS), stages_[kDS].count);

            c->VSSetShaderResources(0, kSrvSlots, vs_srvs_);
            c->PSSetShaderResources(0, kSrvSlots, ps_srvs_);
            c->PSSetSamplers(0, 1, &ps_sampler_);
            if (c1 && vs_cb_) c1->VSSetConstantBuffers1(0, 1, &vs_cb_, &vs_cb_first_, &vs_cb_num_);
            else c->VSSetConstantBuffers(0, 1, &vs_cb_);
            if (c1 && ps_cb_) c1->PSSetConstantBuffers1(0, 1, &ps_cb_, &ps_cb_first_, &ps_cb_num_);
            else c->PSSetConstantBuffers(0, 1, &ps_cb_);

            c->RSSetState(rs_);
            // A count of 0 restores "none bound".
            c->RSSetViewports(vp_count_, vp_count_ ? vps_ : nullptr);
            c->RSSetScissorRects(sc_count_, sc_count_ ? scs_ : nullptr);

            // -1 appends after what the game's stream output already wrote (its offsets cannot be read back).
            UINT so_offsets[D3D11_SO_BUFFER_SLOT_COUNT];
            for (auto& o : so_offsets) o = static_cast<UINT>(-1);
            c->SOSetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so_, so_offsets);
            c->SetPredication(predicate_, predicate_value_);

            ReleaseAll();
        }

    private:
        enum Stage : unsigned { kVS, kPS, kGS, kHS, kDS, kStageCount };
        struct StageInstances {
            std::vector<ID3D11ClassInstance*> instances;
            UINT count = 0;
        };

        template <class Fn>
        void GetStage(Stage s, Fn&& get)
        {
            auto& st = stages_[s];
            st.count = static_cast<UINT>(st.instances.size());
            get(&st.count, st.instances.data());
        }
        ID3D11ClassInstance* const* Instances(Stage s) const
        {
            return stages_[s].count ? stages_[s].instances.data() : nullptr;
        }

        template <class T>
        static void Rel(T*& p)
        {
            if (p) {
                p->Release();
                p = nullptr;
            }
        }

        void ReleaseAll()
        {
            Rel(layout_);
            Rel(vb_);
            Rel(ib_);
            Rel(vs_);
            Rel(ps_);
            Rel(gs_);
            Rel(hs_);
            Rel(ds_);
            for (auto& st : stages_) {
                for (UINT i = 0; i < st.count && i < st.instances.size(); ++i) Rel(st.instances[i]);
                st.count = 0;
            }
            for (auto& p : vs_srvs_) Rel(p);
            for (auto& p : ps_srvs_) Rel(p);
            Rel(ps_sampler_);
            Rel(vs_cb_);
            Rel(ps_cb_);
            Rel(blend_);
            Rel(depth_);
            for (auto& p : rtvs_) Rel(p);
            Rel(dsv_);
            for (auto& p : uavs_) Rel(p);
            Rel(rs_);
            for (auto& p : so_) Rel(p);
            Rel(predicate_);
            captured_ = false;
        }

        bool captured_ = false;
        UINT uav_slots_ = D3D11_PS_CS_UAV_REGISTER_COUNT;

        D3D11_PRIMITIVE_TOPOLOGY topo_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ID3D11InputLayout* layout_ = nullptr;
        ID3D11Buffer* vb_ = nullptr;
        UINT vb_stride_ = 0, vb_offset_ = 0;
        ID3D11Buffer* ib_ = nullptr;
        DXGI_FORMAT ib_format_ = DXGI_FORMAT_UNKNOWN;
        UINT ib_offset_ = 0;

        ID3D11VertexShader* vs_ = nullptr;
        ID3D11PixelShader* ps_ = nullptr;
        ID3D11GeometryShader* gs_ = nullptr;
        ID3D11HullShader* hs_ = nullptr;
        ID3D11DomainShader* ds_ = nullptr;
        StageInstances stages_[kStageCount];

        ID3D11ShaderResourceView* vs_srvs_[kSrvSlots] = {};
        ID3D11ShaderResourceView* ps_srvs_[kSrvSlots] = {};
        ID3D11SamplerState* ps_sampler_ = nullptr;
        ID3D11Buffer* vs_cb_ = nullptr;
        UINT vs_cb_first_ = 0, vs_cb_num_ = 0;
        ID3D11Buffer* ps_cb_ = nullptr;
        UINT ps_cb_first_ = 0, ps_cb_num_ = 0;

        ID3D11BlendState* blend_ = nullptr;
        FLOAT blend_factor_[4] = {};
        UINT sample_mask_ = 0xffffffff;
        ID3D11DepthStencilState* depth_ = nullptr;
        UINT stencil_ref_ = 0;
        ID3D11RenderTargetView* rtvs_[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* dsv_ = nullptr;
        ID3D11UnorderedAccessView* uavs_[D3D11_1_UAV_SLOT_COUNT] = {};

        ID3D11RasterizerState* rs_ = nullptr;
        UINT vp_count_ = 0;
        D3D11_VIEWPORT vps_[kVpSlots] = {};
        UINT sc_count_ = 0;
        D3D11_RECT scs_[kVpSlots] = {};

        ID3D11Buffer* so_[D3D11_SO_BUFFER_SLOT_COUNT] = {};
        ID3D11Predicate* predicate_ = nullptr;
        BOOL predicate_value_ = FALSE;
    };

    // MG: UpdateSubresource, ResolveSubresource and clears are predicated in D3D11. The texture uploads run inside
    // Renderer::Render and the host's resolve in GetTextureSRV, both outside the command list's backup, so a
    // predicate the game left set would silently drop them (an upload lost = a stale glyph/gradient page). This
    // lifts it for the scope and puts it back.
    class ScopedNoPredication {
    public:
        explicit ScopedNoPredication(ID3D11DeviceContext* c) : c_(c)
        {
            c_->GetPredication(&predicate_, &value_);
            if (predicate_) c_->SetPredication(nullptr, FALSE);
        }
        ~ScopedNoPredication()
        {
            if (!predicate_) return;
            c_->SetPredication(predicate_, value_);
            predicate_->Release();
        }
        ScopedNoPredication(const ScopedNoPredication&) = delete;
        ScopedNoPredication& operator=(const ScopedNoPredication&) = delete;

    private:
        ID3D11DeviceContext* c_;
        ID3D11Predicate* predicate_ = nullptr;
        BOOL value_ = FALSE;
    };

    // ── GPUDriverD3D11 (SDK, adapted) ───────────────────────────────────────────
    class GPUDriverD3D11 : public GPUDriverImpl {
    public:
        explicit GPUDriverD3D11(GPUContextD3D11* context) : context_(context) {}
        ~GPUDriverD3D11() override {}

        void CreateTexture(uint32_t texture_id, RefPtr<Bitmap> bitmap, uint32_t flags) override
        {
            if (textures_.find(texture_id) != textures_.end()) {
                LogErrorOnce("MgGpu: CreateTexture, texture id already exists");
                return;
            }
            const bool is_render_target = (flags & kGPUTextureFlag_RenderTarget) != 0;
            const bool is_compressed = bitmap->is_compressed();
            // A block-compressed texture is always immutable (its content path cannot be mapped).
            const bool use_immutable = (flags & kGPUTextureFlag_Immutable) || is_compressed;
            const BitmapFormat format = bitmap->format();
            if (!(format == BitmapFormat::BGRA8_UNORM_SRGB || format == BitmapFormat::A8_UNORM
                  || format == BitmapFormat::RG8_UNORM || IsDataPageFormat(format) || is_compressed)) {
                LogErrorOnce("MgGpu: CreateTexture, unsupported format");
                return;
            }

            D3D11_TEXTURE2D_DESC desc;
            ZeroMemory(&desc, sizeof(desc));
            desc.Width = bitmap->width();
            desc.Height = bitmap->height();
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGIFormatFromBitmapFormat(format, is_render_target);
            desc.SampleDesc.Count = 1;
            // Default usage so UpdateTexture writes only its dirty box (UpdateSubresource); immutable content is
            // created once with its data.
            desc.Usage = use_immutable ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            // Built locally and inserted only once every resource exists, so a failure leaves no half-built entry.
            TextureEntry entry;
            entry.format = desc.Format;
            entry.is_render_target = is_render_target;
            entry.is_immutable = use_immutable && !is_render_target;
            HRESULT hr;
            ID3D11Device* device = context_->device();

            if (is_render_target) {
                desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                desc.Usage = D3D11_USAGE_DEFAULT;
                hr = device->CreateTexture2D(&desc, nullptr, entry.texture.GetAddressOf());
                // MG: an antialiased target draws into its own multisample surface (msaa_texture, the host's
                // sample count) and resolves into `texture`, the single-sample texture everything samples (the
                // engine's binds and the host through GetTextureSRV); the SDK keeps a multisample texture plus a
                // separate resolve texture. Only BGRA targets: single-channel targets are never multisampled.
                const UINT samples = context_->sample_count();
                if (SUCCEEDED(hr) && (flags & kGPUTextureFlag_Antialiased) && samples > 1
                    && desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                    D3D11_TEXTURE2D_DESC ms = desc;
                    ms.SampleDesc.Count = samples;
                    ms.SampleDesc.Quality = 0;
                    ms.BindFlags = D3D11_BIND_RENDER_TARGET;
                    const HRESULT ms_hr = device->CreateTexture2D(&ms, nullptr, entry.msaa_texture.GetAddressOf());
                    if (FAILED(ms_hr)) LogFailure("CreateTexture (MSAA target, drawn without MSAA)", ms_hr, device);
                    else entry.msaa_bytes = TextureBytes(desc.Format, desc.Width, desc.Height) * samples;
                }
                entry.antialiased = (flags & kGPUTextureFlag_Antialiased) != 0;  // MG: MgGpu_GetStats accounting
            } else {
                auto pixels = bitmap->LockPixelsSafe();
                if (pixels.data()) {
                    D3D11_SUBRESOURCE_DATA tex_data;
                    ZeroMemory(&tex_data, sizeof(tex_data));
                    tex_data.pSysMem = pixels.data();
                    tex_data.SysMemPitch = static_cast<UINT>(bitmap->row_bytes());
                    tex_data.SysMemSlicePitch = static_cast<UINT>(bitmap->size());
                    hr = device->CreateTexture2D(&desc, &tex_data, entry.texture.GetAddressOf());
                } else if (!use_immutable) {
                    // MG: no pixels to upload (the SDK would pass a null pointer as initial data, which fails).
                    hr = device->CreateTexture2D(&desc, nullptr, entry.texture.GetAddressOf());
                } else {
                    hr = E_INVALIDARG;
                }
            }
            if (FAILED(hr)) {
                LogFailure("CreateTexture", hr, device);
                return;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
            ZeroMemory(&srv_desc, sizeof(srv_desc));
            srv_desc.Format = desc.Format;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Texture2D.MostDetailedMip = 0;
            srv_desc.Texture2D.MipLevels = 1;
            hr = device->CreateShaderResourceView(entry.texture.Get(), &srv_desc, entry.texture_srv.GetAddressOf());
            if (FAILED(hr)) {
                LogFailure("CreateTexture SRV", hr, device);
                return;
            }
            // MG: MgGpu_GetStats accounting.
            entry.bytes = TextureBytes(desc.Format, desc.Width, desc.Height);
            Account(entry, true);
            if (is_render_target) {
                ++stats_.targetsCreated;
                if (entry.antialiased) ++stats_.antialiasedCreated;
            }
            textures_[texture_id] = std::move(entry);
        }

        // Uploads only the dirty box; the runtime copies the rows before returning.
        void UpdateTexture(uint32_t texture_id, RefPtr<Bitmap> bitmap, const IntRect& dirty_rect) override
        {
            auto i = textures_.find(texture_id);
            if (i == textures_.end()) {
                LogErrorOnce("MgGpu: UpdateTexture, texture id doesn't exist");
                return;
            }
            auto& entry = i->second;
            // MG: an external texture is the host's; a render target has nothing to upload.
            if (entry.is_immutable || entry.is_external || entry.is_render_target || !entry.texture) {
                LogErrorOnce("MgGpu: UpdateTexture, texture cannot be updated (immutable, external or render target)");
                return;
            }
            const IntRect full = { 0, 0, static_cast<int>(bitmap->width()), static_cast<int>(bitmap->height()) };
            const IntRect rect = dirty_rect.IsValid() ? dirty_rect.Intersect(full) : full;
            if (!rect.IsValid()) return;

            auto pixels = bitmap->LockPixelsSafe();
            if (!pixels.data()) return;
            const D3D11_BOX box = { static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0,
                                    static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom), 1 };
            // MG: the source points at the box's first texel, except on a deferred context whose command lists the
            // runtime emulates: that applies the box's offset to the source a second time, so it gets the bitmap's
            // origin instead (Microsoft's documented workaround; tools/deferred-test's upload case on WARP).
            const uint8_t* src = static_cast<const uint8_t*>(pixels.data());
            if (!context_->deferred() || context_->native_lists())
                src += static_cast<size_t>(rect.top) * bitmap->row_bytes() + static_cast<size_t>(rect.left) * bitmap->bpp();
            if (context_->deferred()) {   // MG: a list starts from no predicate, so nothing can drop the upload
                context_->immediate_context()->UpdateSubresource(entry.texture.Get(), 0, &box, src,
                                                                 static_cast<UINT>(bitmap->row_bytes()), 0);
                return;
            }
            const ScopedNoPredication no_predication(context_->immediate_context());
            context_->immediate_context()->UpdateSubresource(entry.texture.Get(), 0, &box, src,
                                                             static_cast<UINT>(bitmap->row_bytes()), 0);
        }

        void DestroyTexture(uint32_t texture_id) override
        {
            auto i = textures_.find(texture_id);
            if (i == textures_.end()) return;
            Account(i->second, false);  // MG: MgGpu_GetStats accounting
            textures_.erase(i);
        }

        void CreateRenderBuffer(uint32_t render_buffer_id, const RenderBuffer& buffer) override
        {
            if (render_buffer_id == 0) {
                LogErrorOnce("MgGpu: CreateRenderBuffer, id 0 is reserved");
                return;
            }
            if (render_targets_.find(render_buffer_id) != render_targets_.end()) {
                LogErrorOnce("MgGpu: CreateRenderBuffer, id already exists");
                return;
            }
            auto tex_entry = textures_.find(buffer.texture_id);
            if (tex_entry == textures_.end() || !tex_entry->second.is_render_target) {
                LogErrorOnce("MgGpu: CreateRenderBuffer, texture id doesn't exist or is not a render target");
                return;
            }
            auto& tex = tex_entry->second;
            ID3D11Device* device = context_->device();

            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc;
            ZeroMemory(&rtv_desc, sizeof(rtv_desc));
            rtv_desc.Format = tex.format;
            const bool msaa = tex.msaa_texture != nullptr;
            rtv_desc.ViewDimension = msaa ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;

            RenderTargetEntry rt_entry;
            HRESULT hr = device->CreateRenderTargetView(msaa ? tex.msaa_texture.Get() : tex.texture.Get(), &rtv_desc,
                                                        rt_entry.render_target_view.GetAddressOf());
            if (FAILED(hr) && msaa) {
                // MG: the target draws without MSAA, the same state as a multisample texture that failed to create.
                LogFailure("CreateRenderBuffer (MSAA view, drawn without MSAA)", hr, device);
                // MG: MgGpu_GetStats accounting: uncount the entry, drop its surface, count it again.
                Account(tex, false);
                tex.msaa_texture.Reset();
                tex.msaa_bytes = 0;
                Account(tex, true);
                tex.needs_resolve = false;
                rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                hr = device->CreateRenderTargetView(tex.texture.Get(), &rtv_desc,
                                                    rt_entry.render_target_view.GetAddressOf());
            }
            if (FAILED(hr)) {
                LogFailure("CreateRenderBuffer RTV", hr, device);
                return;
            }
            rt_entry.render_target_texture_id = buffer.texture_id;
            render_targets_[render_buffer_id] = std::move(rt_entry);
        }

        void DestroyRenderBuffer(uint32_t render_buffer_id) override { render_targets_.erase(render_buffer_id); }

        void CreateGeometry(uint32_t geometry_id, const VertexBuffer& vertices, const IndexBuffer& indices) override
        {
            // MG: no BindVertexLayout here (the SDK binds the input layout at creation): this runs inside
            // Renderer::Render, outside the command list's state backup, and would leave it on the game's context.
            if (geometry_.find(geometry_id) != geometry_.end()) return;
            GeometryEntry geometry;
            geometry.format = vertices.format;
            if (!CreateDynamicBuffer(D3D11_BIND_VERTEX_BUFFER, vertices.data, vertices.size, geometry.vertexBuffer,
                                     geometry.vertex_capacity)
                || !CreateDynamicBuffer(D3D11_BIND_INDEX_BUFFER, indices.data, indices.size, geometry.indexBuffer,
                                        geometry.index_capacity))
                return;
            geometry_.insert({ geometry_id, std::move(geometry) });
        }

        void UpdateGeometry(uint32_t geometry_id, const VertexBuffer& vertices, const IndexBuffer& indices) override
        {
            auto i = geometry_.find(geometry_id);
            if (i == geometry_.end()) {
                LogErrorOnce("MgGpu: UpdateGeometry, geometry id doesn't exist");
                return;
            }
            auto& entry = i->second;
            entry.format = vertices.format;
            WriteDynamicBuffer(D3D11_BIND_VERTEX_BUFFER, vertices.data, vertices.size, entry.vertexBuffer,
                               entry.vertex_capacity);
            WriteDynamicBuffer(D3D11_BIND_INDEX_BUFFER, indices.data, indices.size, entry.indexBuffer,
                               entry.index_capacity);
        }

        void DestroyGeometry(uint32_t geometry_id) override { geometry_.erase(geometry_id); }

        void GetDeviceCaps(GPUDeviceCaps& caps) override { context_->FillCaps(caps); }

        void BindTexture(uint8_t texture_unit, uint32_t texture_id) override
        {
            ID3D11ShaderResourceView* srv = SampleSRV(texture_id);
            if (!srv) return;
            // Mirrored to the vertex stage: FillPhotonGrid's vertex shader reads the data pages.
            context_->immediate_context()->PSSetShaderResources(texture_unit, 1, &srv);
            context_->immediate_context()->VSSetShaderResources(texture_unit, 1, &srv);
        }

        void ClearRenderBuffer(uint32_t render_buffer_id) override
        {
            bound_.valid = false;
            const float color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            ID3D11RenderTargetView* target = GetRenderTargetView(render_buffer_id);
            if (!target) {
                LogErrorOnce("MgGpu: ClearRenderBuffer, render buffer id doesn't exist");
                return;
            }
            context_->immediate_context()->ClearRenderTargetView(target, color);
        }

        // Some driver stacks reorder sampling draws against a following clear/draw of the same texture; submitting
        // the pending work closes that window. MG: a deferred context has nothing to submit (a list cannot record a
        // Flush); its commands run in order when the list does.
        void Flush() override
        {
            if (!context_->deferred()) context_->immediate_context()->Flush();
        }

        void DrawGeometry(uint32_t geometry_id, uint32_t indices_count, uint32_t indices_offset,
                          const GPUState& state) override
        {
            auto* ctx = context_->immediate_context();
            const bool full = !bound_.valid;
            if (full) bound_.has_scissor_rect = false;

            if (full || state.render_buffer_id != bound_.render_buffer_id) {
                // Also unbinds every texture slot. MG: a missing target skips the draw: the bound target is then
                // still the previous one, or the GAME's at the start of a list.
                if (!BindRenderBuffer(state.render_buffer_id)) {
                    bound_.valid = false;
                    return;
                }
                bound_.render_buffer_id = state.render_buffer_id;
                std::fill(std::begin(bound_.texture_id), std::end(bound_.texture_id), 0u);
            }

            if (full || state.viewport_width != bound_.viewport_width
                || state.viewport_height != bound_.viewport_height) {
                SetViewport(state.viewport_width, state.viewport_height);
                bound_.viewport_width = state.viewport_width;
                bound_.viewport_height = state.viewport_height;
            }

            // Never sample the texture behind the bound render target. The core can name it (a stale id after
            // pooled render-texture reuse); MG: with MSAA, binding it would also resolve the target mid-layer and
            // clear needs_resolve, so the layer's last draws would never reach the resolve texture (a missing
            // border segment, msaa 4, translucent-borders.html). A slot whose id is 0, skipped or unknown is null.
            const uint32_t rt_tex = bound_render_target_texture_id_;
            const uint32_t texture_ids[4] = { state.texture_1_id, state.texture_2_id, state.texture_3_id,
                                              state.texture_4_id };
            for (UINT slot = 0; slot < 4; ++slot) {
                const uint32_t id = texture_ids[slot] != rt_tex ? texture_ids[slot] : 0;
                if (id == bound_.texture_id[slot]) continue;
                ID3D11ShaderResourceView* srv = id ? SampleSRV(id) : nullptr;
                ctx->PSSetShaderResources(slot, 1, &srv);
                ctx->VSSetShaderResources(slot, 1, &srv);
                bound_.texture_id[slot] = id;
            }

            if (full || !SameUniforms(state, bound_.uniform_state)) {
                if (!UpdateConstantBuffer(state)) {
                    bound_.valid = false;
                    return;
                }
                bound_.uniform_state = state;
            }

            if (full || geometry_id != bound_.geometry_id) {
                if (!BindGeometry(geometry_id)) {
                    // MG: never draw with whatever vertex/index buffers happen to be bound.
                    LogErrorOnce("MgGpu: DrawGeometry, geometry id doesn't exist");
                    bound_.valid = false;
                    return;
                }
                bound_.geometry_id = geometry_id;
            }

            if (full) {
                ID3D11SamplerState* sampler = GetSamplerState();
                ctx->PSSetSamplers(0, 1, &sampler);
            }

            if (full || state.shader_type != bound_.shader_type) {
                if (!BindShader(state.shader_type)) {
                    LogErrorOnce("MgGpu: DrawGeometry, shader program unavailable");
                    bound_.valid = false;
                    return;
                }
                bound_.shader_type = state.shader_type;
            }

            if (full || state.enable_blend != bound_.enable_blend
                || (state.enable_blend
                    && (state.blend_src_factor != bound_.blend_src_factor
                        || state.blend_dst_factor != bound_.blend_dst_factor
                        || state.blend_equation != bound_.blend_equation))) {
                if (state.enable_blend)
                    context_->SetBlendState(static_cast<uint8_t>(state.blend_src_factor),
                                            static_cast<uint8_t>(state.blend_dst_factor),
                                            static_cast<uint8_t>(state.blend_equation));
                else
                    context_->DisableBlend();
                bound_.enable_blend = state.enable_blend;
                bound_.blend_src_factor = state.blend_src_factor;
                bound_.blend_dst_factor = state.blend_dst_factor;
                bound_.blend_equation = state.blend_equation;
            }

            if (full || state.enable_scissor != bound_.enable_scissor) {
                if (state.enable_scissor) context_->EnableScissor();
                else context_->DisableScissor();
                bound_.enable_scissor = state.enable_scissor;
            }
            if (state.enable_scissor && (!bound_.has_scissor_rect || !(state.scissor_rect == bound_.scissor_rect))) {
                const D3D11_RECT scissor_rect = { static_cast<LONG>(state.scissor_rect.left),
                                                  static_cast<LONG>(state.scissor_rect.top),
                                                  static_cast<LONG>(state.scissor_rect.right),
                                                  static_cast<LONG>(state.scissor_rect.bottom) };
                ctx->RSSetScissorRects(1, &scissor_rect);
                bound_.has_scissor_rect = true;
                bound_.scissor_rect = state.scissor_rect;
            }

            if (full) {
                ctx->VSSetConstantBuffers(0, 1, constant_buffer_.GetAddressOf());
                ctx->PSSetConstantBuffers(0, 1, constant_buffer_.GetAddressOf());
            }
            ctx->DrawIndexed(indices_count, indices_offset, 0);
            batch_count_++;
            bound_.valid = bound_.active;
        }

        // MG: the command list runs between a capture and a restore of the game's pipeline state. A deferred
        // context holds no state of the game's: the host captures and restores around running the list instead.
        void DrawCommandList() override
        {
            if (command_list_.empty()) return;
            const bool game_state = !context_->deferred();
            if (game_state) {
                backup_.Capture(*context_);
                PipelineBackup::Neutralize(context_->immediate_context());
            }
            bound_.active = true;
            bound_.valid = false;
            GPUDriverImpl::DrawCommandList();
            bound_.active = false;
            bound_.valid = false;
            if (game_state) backup_.Restore(*context_);
        }

        // MG: host compositor support — the SRV for a View's target texture, resolved first when it is drawn
        // with MSAA. Render thread, after DrawCommandList.
        ID3D11ShaderResourceView* GetTextureSRV(uint32_t texture_id)
        {
            auto i = textures_.find(texture_id);
            if (i == textures_.end()) return nullptr;
            if (i->second.msaa_texture && i->second.needs_resolve) {
                if (context_->deferred()) {   // MG: recorded; a list starts from no predicate
                    Resolve(i->second);
                } else {
                    const ScopedNoPredication no_predication(context_->immediate_context());
                    Resolve(i->second);
                }
            }
            return i->second.texture_srv.Get();
        }

        // MG: external textures (ImageSource). An SRV the host owns, entered under a fresh id from the SAME counter
        // Ultralight's CreateTexture draws on (a hand-picked id would alias its next texture). The draw binds then
        // serve it like any driver texture; Ultralight never uploads to it.
        uint32_t RegisterExternal(ID3D11ShaderResourceView* srv)
        {
            if (!srv) return 0;
            const uint32_t id = NextTextureId();
            SetExternal(textures_[id], srv);
            return id;
        }
        bool SetExternalSRV(uint32_t texture_id, ID3D11ShaderResourceView* srv)
        {
            auto i = textures_.find(texture_id);
            if (i == textures_.end() || !srv) return false;
            Account(i->second, false);   // an engine texture re-pointed here stops counting as the engine's
            i->second.bytes = i->second.msaa_bytes = 0;
            i->second.antialiased = false;
            SetExternal(i->second, srv);
            bound_.valid = false;  // a cached slot bind of this id now names another view
            return true;
        }
        void UnregisterExternal(uint32_t texture_id)
        {
            auto i = textures_.find(texture_id);
            if (i == textures_.end()) return;
            Account(i->second, false);   // 0 for an external entry; an engine id passed here stops counting too
            textures_.erase(i);
        }

        // MG: what MgGpu_GetStats reports (Account keeps the live counts and bytes).
        MgGpuStats Stats() const
        {
            MgGpuStats s = stats_;
            s.flushes = flushes();
            return s;
        }

    private:
        struct TextureEntry {
            ComPtr<ID3D11Texture2D> texture;  // MG: single-sample; the resolve target of an MSAA render target
            ComPtr<ID3D11ShaderResourceView> texture_srv;
            // The texture's DXGI format; render-target views and resolves must match it (an A8 target is R8).
            DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
            bool is_render_target = false;
            bool is_immutable = false;
            bool is_external = false;  // MG: an SRV the host registered
            // MG: an MSAA render target's multisample surface, and whether it was drawn since the last resolve.
            ComPtr<ID3D11Texture2D> msaa_texture;
            bool needs_resolve = false;
            // MG: for MgGpu_GetStats; an external texture counts 0 bytes (the host's allocation).
            bool antialiased = false;  // created with kGPUTextureFlag_Antialiased
            std::uint64_t bytes = 0;
            std::uint64_t msaa_bytes = 0;
        };

        // MG: bytes of one mip level of a 2D texture in a format CreateTexture accepts.
        static std::uint64_t TextureBytes(DXGI_FORMAT format, UINT w, UINT h)
        {
            const std::uint64_t blocks = std::uint64_t((w + 3) / 4) * ((h + 3) / 4);
            switch (format) {
            case DXGI_FORMAT_BC1_UNORM: return blocks * 8;
            case DXGI_FORMAT_BC2_UNORM:
            case DXGI_FORMAT_BC3_UNORM:
            case DXGI_FORMAT_BC7_UNORM: return blocks * 16;
            case DXGI_FORMAT_A8_UNORM:
            case DXGI_FORMAT_R8_UNORM: return std::uint64_t(w) * h;
            case DXGI_FORMAT_R8G8_UNORM: return std::uint64_t(w) * h * 2;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_UINT: return std::uint64_t(w) * h * 8;
            case DXGI_FORMAT_R32G32B32A32_FLOAT: return std::uint64_t(w) * h * 16;
            default: return std::uint64_t(w) * h * 4;
            }
        }

        // MG: adds (or takes away) one entry's share of the live counts.
        void Account(const TextureEntry& e, bool add)
        {
            const auto step = [add](auto& n, auto by) { n = add ? n + by : n - by; };
            if (e.is_render_target) {
                step(stats_.renderTargets, 1u);
                if (e.antialiased) step(stats_.antialiasedTargets, 1u);
                if (e.msaa_texture) step(stats_.msaaSurfaces, 1u);
                step(stats_.targetBytes, e.bytes);
                step(stats_.msaaBytes, e.msaa_bytes);
            } else {
                step(stats_.textureBytes, e.bytes);
            }
        }

        struct GeometryEntry {
            VertexBufferFormat format = VertexBufferFormat::_2f_4ub_2f;
            ComPtr<ID3D11Buffer> vertexBuffer;
            ComPtr<ID3D11Buffer> indexBuffer;
            uint32_t vertex_capacity = 0;  // MG: ByteWidth, so an update that outgrows it reallocates
            uint32_t index_capacity = 0;
        };

        struct RenderTargetEntry {
            ComPtr<ID3D11RenderTargetView> render_target_view;
            uint32_t render_target_texture_id = 0;
        };

        // What DrawGeometry last bound, so a draw skips the binds (and the constant upload) its predecessor
        // already made. Valid only inside DrawCommandList; a clear invalidates it (it flags an MSAA resolve that a
        // skipped texture bind would miss).
        struct BoundState {
            bool active = false;
            bool valid = false;
            uint32_t render_buffer_id = 0;
            uint32_t texture_id[4] = {};
            uint32_t viewport_width = 0;
            uint32_t viewport_height = 0;
            uint32_t geometry_id = 0;
            ShaderType shader_type = ShaderType::Fill;
            bool enable_blend = false;
            BlendFactor blend_src_factor = BlendFactor::One;
            BlendFactor blend_dst_factor = BlendFactor::One;
            BlendEquation blend_equation = BlendEquation::Add;
            bool enable_scissor = false;
            bool has_scissor_rect = false;
            IntRect scissor_rect = {};
            GPUState uniform_state = {};
        };

        static DXGI_FORMAT DXGIFormatFromBitmapFormat(BitmapFormat format, bool is_render_target)
        {
            // A single-channel render target backs onto R8 (A8 is not universally renderable, and the shaders read
            // a render target's coverage from red); sampled A8 keeps the classic A8 mapping (coverage in alpha).
            if (is_render_target && format == BitmapFormat::A8_UNORM) return DXGI_FORMAT_R8_UNORM;
            switch (format) {
            case BitmapFormat::A8_UNORM:         return DXGI_FORMAT_A8_UNORM;
            case BitmapFormat::BGRA8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case BitmapFormat::RG8_UNORM:        return DXGI_FORMAT_R8G8_UNORM;
            // BCn as plain UNORM, like BGRA8_UNORM_SRGB: the renderer blends the stored sRGB values as-is.
            case BitmapFormat::BC1_UNORM:        return DXGI_FORMAT_BC1_UNORM;
            case BitmapFormat::BC2_UNORM:        return DXGI_FORMAT_BC2_UNORM;
            case BitmapFormat::BC3_UNORM:        return DXGI_FORMAT_BC3_UNORM;
            case BitmapFormat::BC7_UNORM:        return DXGI_FORMAT_BC7_UNORM;
            case BitmapFormat::RGBA16F:          return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case BitmapFormat::RGBA16UI:         return DXGI_FORMAT_R16G16B16A16_UINT;
            case BitmapFormat::RGBA32F:          return DXGI_FORMAT_R32G32B32A32_FLOAT;
            }
            return DXGI_FORMAT_A8_UNORM;
        }

        // The typed data-page formats (raw texels read with texel loads; never filtered, never mipmapped).
        static bool IsDataPageFormat(BitmapFormat format)
        {
            return format == BitmapFormat::RGBA16F || format == BitmapFormat::RGBA16UI
                || format == BitmapFormat::RGBA32F;
        }

        // True when `a` and `b` fill the constant buffer identically (the shaders read only clip_size clips).
        static bool SameUniforms(const GPUState& a, const GPUState& b)
        {
            if (a.viewport_width != b.viewport_width || a.viewport_height != b.viewport_height
                || a.clip_size != b.clip_size)
                return false;
            const size_t num_clips = (std::min)(static_cast<size_t>(a.clip_size), std::size(a.clip));
            return !std::memcmp(&a.transform, &b.transform, sizeof(a.transform))
                && !std::memcmp(a.uniform_integer, b.uniform_integer, sizeof(a.uniform_integer))
                && !std::memcmp(a.uniform_scalar, b.uniform_scalar, sizeof(a.uniform_scalar))
                && !std::memcmp(a.uniform_vector, b.uniform_vector, sizeof(a.uniform_vector))
                && !std::memcmp(a.clip, b.clip, num_clips * sizeof(a.clip[0]));
        }

        bool CreateDynamicBuffer(UINT bind, const void* data, uint32_t size, ComPtr<ID3D11Buffer>& out,
                                 uint32_t& capacity)
        {
            out.Reset();
            capacity = 0;
            if (!size || !data) return false;
            D3D11_BUFFER_DESC desc;
            ZeroMemory(&desc, sizeof(desc));
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.ByteWidth = size;
            desc.BindFlags = bind;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            D3D11_SUBRESOURCE_DATA init;
            ZeroMemory(&init, sizeof(init));
            init.pSysMem = data;
            const HRESULT hr = context_->device()->CreateBuffer(&desc, &init, out.GetAddressOf());
            if (FAILED(hr)) {
                LogFailure(bind == D3D11_BIND_VERTEX_BUFFER ? "CreateGeometry (vertex buffer)"
                                                            : "CreateGeometry (index buffer)",
                           hr, context_->device());
                return false;
            }
            capacity = size;
            return true;
        }

        // MG: the SDK maps without checking (a failed map on a removed device writes through garbage) and never
        // checks the size against the buffer's.
        void WriteDynamicBuffer(UINT bind, const void* data, uint32_t size, ComPtr<ID3D11Buffer>& buffer,
                                uint32_t& capacity)
        {
            if (!size || !data) return;
            if (!buffer || size > capacity) {
                CreateDynamicBuffer(bind, data, size, buffer, capacity);
                return;
            }
            D3D11_MAPPED_SUBRESOURCE res;
            const HRESULT hr = context_->immediate_context()->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res);
            if (FAILED(hr)) {
                LogFailure("UpdateGeometry Map", hr, context_->device());
                return;
            }
            std::memcpy(res.pData, data, size);
            context_->immediate_context()->Unmap(buffer.Get(), 0);
        }

        void LoadCompiledVertexShader(const unsigned char* data, unsigned int len, ID3D11VertexShader** vs,
                                      const D3D11_INPUT_ELEMENT_DESC* il, UINT il_count, ID3D11InputLayout** layout)
        {
            HRESULT hr = context_->device()->CreateVertexShader(data, len, nullptr, vs);
            if (FAILED(hr)) {
                LogFailure("CreateVertexShader", hr, context_->device());
                return;
            }
            if (layout && !*layout) {
                hr = context_->device()->CreateInputLayout(il, il_count, data, len, layout);
                if (FAILED(hr)) LogFailure("CreateInputLayout", hr, context_->device());
            }
        }

        void LoadCompiledPixelShader(const unsigned char* data, unsigned int len, ID3D11PixelShader** ps)
        {
            const HRESULT hr = context_->device()->CreatePixelShader(data, len, nullptr, ps);
            if (FAILED(hr)) LogFailure("CreatePixelShader", hr, context_->device());
        }

        // MG: shaders always come from the embedded bytecode (no file system), and the six quad-layout programs
        // share one vertex shader object (the SDK creates a copy per program).
        void LoadShaders()
        {
            if (shaders_loaded_) return;
            shaders_loaded_ = true;

            const D3D11_INPUT_ELEMENT_DESC layout_2f_4ub_2f[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            const D3D11_INPUT_ELEMENT_DESC layout_2f_4ub_2f_2f_28f[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 6, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR", 7, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            const D3D11_INPUT_ELEMENT_DESC layout_2f_2ui[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32_UINT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };

            LoadCompiledVertexShader(vertex_path_vs_data, vertex_path_vs_size, vs_path_.GetAddressOf(),
                                     layout_2f_4ub_2f, ARRAYSIZE(layout_2f_4ub_2f),
                                     vertex_layout_2f_4ub_2f_.GetAddressOf());
            LoadCompiledVertexShader(vertex_quad_vs_data, vertex_quad_vs_size, vs_quad_.GetAddressOf(),
                                     layout_2f_4ub_2f_2f_28f, ARRAYSIZE(layout_2f_4ub_2f_2f_28f),
                                     vertex_layout_2f_4ub_2f_2f_28f_.GetAddressOf());
            LoadCompiledVertexShader(vertex_photon_grid_vs_data, vertex_photon_grid_vs_size,
                                     vs_photon_grid_.GetAddressOf(), layout_2f_2ui, ARRAYSIZE(layout_2f_2ui),
                                     vertex_layout_2f_2ui_.GetAddressOf());

            struct PsSource {
                ShaderType type;
                const unsigned char* data;
                unsigned int size;
                ID3D11VertexShader* vs;
            };
            const PsSource programs[] = {
                { ShaderType::Fill, fill_ps_data, fill_ps_size, vs_quad_.Get() },
                { ShaderType::FillPath, fill_path_ps_data, fill_path_ps_size, vs_path_.Get() },
                { ShaderType::FilterBasic, filter_basic_ps_data, filter_basic_ps_size, vs_quad_.Get() },
                { ShaderType::FilterBlur, filter_blur_ps_data, filter_blur_ps_size, vs_quad_.Get() },
                { ShaderType::FilterDropShadow, filter_dropshadow_ps_data, filter_dropshadow_ps_size, vs_quad_.Get() },
                { ShaderType::FillPhoton, fill_photon_ps_data, fill_photon_ps_size, vs_quad_.Get() },
                { ShaderType::FillPhotonGrid, fill_photon_grid_ps_data, fill_photon_grid_ps_size,
                  vs_photon_grid_.Get() },
            };
            for (const auto& p : programs) {
                auto& program = shaders_[p.type];
                program.first = p.vs;
                LoadCompiledPixelShader(p.data, p.size, program.second.GetAddressOf());
            }
        }

        bool BindShader(ShaderType shader_type)
        {
            LoadShaders();
            auto i = shaders_.find(shader_type);
            if (i == shaders_.end() || !i->second.first || !i->second.second) return false;
            context_->immediate_context()->VSSetShader(i->second.first.Get(), nullptr, 0);
            context_->immediate_context()->PSSetShader(i->second.second.Get(), nullptr, 0);
            return true;
        }

        bool BindGeometry(uint32_t id)
        {
            auto i = geometry_.find(id);
            if (i == geometry_.end()) return false;
            LoadShaders();
            auto* ctx = context_->immediate_context();
            auto& geometry = i->second;
            UINT stride;
            ID3D11InputLayout* layout;
            switch (geometry.format) {
            case VertexBufferFormat::_2f_4ub_2f:
                stride = sizeof(Vertex_2f_4ub_2f);
                layout = vertex_layout_2f_4ub_2f_.Get();
                break;
            case VertexBufferFormat::_2f_2ui:
                stride = sizeof(Vertex_2f_2ui);
                layout = vertex_layout_2f_2ui_.Get();
                break;
            default:
                stride = sizeof(Vertex_2f_4ub_2f_2f_28f);
                layout = vertex_layout_2f_4ub_2f_2f_28f_.Get();
                break;
            }
            if (!layout) return false;
            const UINT offset = 0;
            ctx->IASetVertexBuffers(0, 1, geometry.vertexBuffer.GetAddressOf(), &stride, &offset);
            ctx->IASetIndexBuffer(geometry.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->IASetInputLayout(layout);
            return true;
        }

        // Unbinds every texture slot (a target cannot be bound while sampled), then binds the target.
        bool BindRenderBuffer(uint32_t render_buffer_id)
        {
            auto* ctx = context_->immediate_context();
            ID3D11ShaderResourceView* null_srv[4] = {};
            ctx->PSSetShaderResources(0, 4, null_srv);
            ctx->VSSetShaderResources(0, 4, null_srv);

            bound_render_target_texture_id_ = 0;
            ID3D11RenderTargetView* target = GetRenderTargetView(render_buffer_id);
            if (!target) {
                LogErrorOnce("MgGpu: BindRenderBuffer, render buffer id doesn't exist");
                return false;
            }
            bound_render_target_texture_id_ = render_targets_[render_buffer_id].render_target_texture_id;
            ctx->OMSetRenderTargets(1, &target, nullptr);
            return true;
        }

        ID3D11RenderTargetView* GetRenderTargetView(uint32_t render_buffer_id)
        {
            // MG: no swap-chain fallback — offscreen render buffers only.
            auto i = render_targets_.find(render_buffer_id);
            if (i == render_targets_.end()) return nullptr;
            // Bound to be drawn or cleared: an MSAA target needs a resolve before it is next sampled.
            auto t = textures_.find(i->second.render_target_texture_id);
            if (t == textures_.end()) {
                LogErrorOnce("MgGpu: render target texture id doesn't exist");
                return nullptr;
            }
            if (t->second.msaa_texture) t->second.needs_resolve = true;
            return i->second.render_target_view.Get();
        }

        // The view a draw samples `texture_id` through (an MSAA render target is resolved first), or null.
        ID3D11ShaderResourceView* SampleSRV(uint32_t texture_id)
        {
            auto i = textures_.find(texture_id);
            if (i == textures_.end()) {
                LogErrorOnce("MgGpu: BindTexture, texture id doesn't exist");
                return nullptr;
            }
            Resolve(i->second);
            return i->second.texture_srv.Get();
        }

        void Resolve(TextureEntry& e)
        {
            if (!e.msaa_texture || !e.needs_resolve) return;
            context_->immediate_context()->ResolveSubresource(e.texture.Get(), 0, e.msaa_texture.Get(), 0, e.format);
            e.needs_resolve = false;
        }

        // MG: external-texture helper.
        static void SetExternal(TextureEntry& e, ID3D11ShaderResourceView* srv)
        {
            e.msaa_texture.Reset();
            e.needs_resolve = false;
            e.is_external = true;
            e.is_render_target = false;
            e.is_immutable = false;
            e.texture_srv = srv;  // ComPtr assignment AddRefs
            D3D11_SHADER_RESOURCE_VIEW_DESC desc;
            srv->GetDesc(&desc);
            e.format = desc.Format;
            ComPtr<ID3D11Resource> res;
            srv->GetResource(res.GetAddressOf());
            e.texture.Reset();
            if (res) res.As(&e.texture);  // null for a non-Texture2D resource; only the SRV is ever bound
        }

        ID3D11SamplerState* GetSamplerState()
        {
            if (sampler_state_) return sampler_state_.Get();
            D3D11_SAMPLER_DESC desc;
            ZeroMemory(&desc, sizeof(desc));
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            desc.MinLOD = 0;
            const HRESULT hr = context_->device()->CreateSamplerState(&desc, sampler_state_.GetAddressOf());
            if (FAILED(hr)) LogFailure("CreateSamplerState", hr, context_->device());
            return sampler_state_.Get();
        }

        void SetViewport(uint32_t width, uint32_t height)
        {
            D3D11_VIEWPORT vp;
            ZeroMemory(&vp, sizeof(vp));
            vp.Width = static_cast<float>(width);
            vp.Height = static_cast<float>(height);
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            context_->immediate_context()->RSSetViewports(1, &vp);
        }

        bool UpdateConstantBuffer(const GPUState& state)
        {
            if (!constant_buffer_) {
                D3D11_BUFFER_DESC desc;
                ZeroMemory(&desc, sizeof(desc));
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.ByteWidth = sizeof(Uniforms);
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                const HRESULT hr = context_->device()->CreateBuffer(&desc, nullptr, constant_buffer_.GetAddressOf());
                if (FAILED(hr)) {
                    LogFailure("CreateBuffer (constants)", hr, context_->device());
                    return false;
                }
            }

            const Matrix mvp = ApplyProjection(state.transform, static_cast<float>(state.viewport_width),
                                               static_cast<float>(state.viewport_height));
            Uniforms u;
            u.State = { 0.0f, static_cast<float>(state.viewport_width), static_cast<float>(state.viewport_height),
                        1.0f };
            u.Transform = DirectX::XMMATRIX(mvp.GetMatrix4x4().data);
            u.Integer4[0] = { state.uniform_integer[0], state.uniform_integer[1], state.uniform_integer[2],
                              state.uniform_integer[3] };
            u.Integer4[1] = { state.uniform_integer[4], state.uniform_integer[5], state.uniform_integer[6],
                              state.uniform_integer[7] };
            u.Scalar4[0] = { state.uniform_scalar[0], state.uniform_scalar[1], state.uniform_scalar[2],
                             state.uniform_scalar[3] };
            u.Scalar4[1] = { state.uniform_scalar[4], state.uniform_scalar[5], state.uniform_scalar[6],
                             state.uniform_scalar[7] };
            for (size_t i = 0; i < 8; ++i)
                u.Vector[i] = DirectX::XMFLOAT4(state.uniform_vector[i].x, state.uniform_vector[i].y,
                                                state.uniform_vector[i].z, state.uniform_vector[i].w);
            u.ClipData = { static_cast<int32_t>(state.clip_size), 0, 0, 0 };
            const size_t clips = (std::min)(static_cast<size_t>(state.clip_size), std::size(state.clip));
            for (size_t i = 0; i < clips; ++i) u.Clip[i] = DirectX::XMMATRIX(state.clip[i].data);

            D3D11_MAPPED_SUBRESOURCE res;
            const HRESULT hr = context_->immediate_context()->Map(constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD,
                                                                  0, &res);
            if (FAILED(hr)) {
                LogFailure("Map (constants)", hr, context_->device());
                return false;
            }
            std::memcpy(res.pData, &u, sizeof(Uniforms));
            context_->immediate_context()->Unmap(constant_buffer_.Get(), 0);
            return true;
        }

        static Matrix ApplyProjection(const Matrix4x4& transform, float screen_width, float screen_height)
        {
            Matrix transform_mat;
            transform_mat.Set(transform);
            Matrix result;
            result.SetOrthographicProjection(screen_width, screen_height, false);
            result.Transform(transform_mat);
            return result;
        }

        GPUContextD3D11* context_;
        PipelineBackup backup_;
        bool shaders_loaded_ = false;
        ComPtr<ID3D11VertexShader> vs_path_;
        ComPtr<ID3D11VertexShader> vs_quad_;
        ComPtr<ID3D11VertexShader> vs_photon_grid_;
        ComPtr<ID3D11InputLayout> vertex_layout_2f_4ub_2f_;
        ComPtr<ID3D11InputLayout> vertex_layout_2f_4ub_2f_2f_28f_;
        ComPtr<ID3D11InputLayout> vertex_layout_2f_2ui_;
        ComPtr<ID3D11SamplerState> sampler_state_;
        ComPtr<ID3D11Buffer> constant_buffer_;
        std::map<uint32_t, GeometryEntry> geometry_;
        std::map<uint32_t, TextureEntry> textures_;
        std::map<uint32_t, RenderTargetEntry> render_targets_;
        std::map<ShaderType, std::pair<ComPtr<ID3D11VertexShader>, ComPtr<ID3D11PixelShader>>> shaders_;
        // The texture behind the bound render target (0 when none): DrawGeometry never sample-binds it.
        uint32_t bound_render_target_texture_id_ = 0;
        BoundState bound_;
        MgGpuStats stats_{};  // MG: live counts (Account) and creation totals; the flush count is the base's
    };

}  // namespace mggpu

// ── C ABI (MG) ──────────────────────────────────────────────────────────────────

namespace {
    struct MgGpuHandle {
        mggpu::GPUContextD3D11* context;
        mggpu::GPUDriverD3D11* driver;
    };
}

extern "C" {

    const MgGpuInfo* MgGpu_GetInfo()
    {
        static const MgGpuInfo info{
            static_cast<std::uint32_t>(sizeof(MgGpuInfo)),
            MGGPU_CONTRACT,
            static_cast<std::uint32_t>(sizeof(ultralight::GPUState)),
            static_cast<std::uint32_t>(sizeof(ultralight::Command)),
            ULTRALIGHT_VERSION,
        };
        return &info;
    }

    void* MgGpu_Create(ID3D11Device* device, ID3D11DeviceContext* context, MgGpuLogFn log)
    {
        if (!device || !context) return nullptr;
        g_log = log;
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            g_logSeen.clear();
        }
        auto* h = new MgGpuHandle();
        h->context = new mggpu::GPUContextD3D11(device, context);
        h->driver = new mggpu::GPUDriverD3D11(h->context);
        return h;
    }

    void MgGpu_Destroy(void* handle)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        if (!h) return;
        delete h->driver;
        delete h->context;
        delete h;
    }

    void* MgGpu_GetGPUDriver(void* handle)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        return h ? static_cast<ultralight::GPUDriver*>(h->driver) : nullptr;
    }

    int MgGpu_HasCommandsPending(void* handle)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        return (h && h->driver->HasCommandsPending()) ? 1 : 0;
    }

    void MgGpu_DrawCommandList(void* handle)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        if (h) h->driver->DrawCommandList();
    }

    int MgGpu_SetSampleCount(void* handle, int samples)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        return h ? static_cast<int>(h->context->SetSampleCount(samples > 0 ? static_cast<UINT>(samples) : 1u)) : 0;
    }

    void* MgGpu_GetTextureSRV(void* handle, std::uint32_t texture_id)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        return h ? h->driver->GetTextureSRV(texture_id) : nullptr;
    }

    std::uint32_t MgGpu_RegisterExternalTexture(void* handle, ID3D11ShaderResourceView* srv)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        return h ? h->driver->RegisterExternal(srv) : 0;
    }

    int MgGpu_SetExternalTextureSRV(void* handle, std::uint32_t texture_id, ID3D11ShaderResourceView* srv)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        return (h && h->driver->SetExternalSRV(texture_id, srv)) ? 1 : 0;
    }

    void MgGpu_UnregisterExternalTexture(void* handle, std::uint32_t texture_id)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        if (h) h->driver->UnregisterExternal(texture_id);
    }

    int MgGpu_GetStats(void* handle, MgGpuStats* out)
    {
        auto* h = static_cast<MgGpuHandle*>(handle);
        if (!h || !out || out->size < sizeof(std::uint32_t)) return 0;
        MgGpuStats s = h->driver->Stats();
        const std::uint32_t n = (std::min)(out->size, static_cast<std::uint32_t>(sizeof(MgGpuStats)));
        s.size = n;
        std::memcpy(out, &s, n);
        return 1;
    }

}  // extern "C"
