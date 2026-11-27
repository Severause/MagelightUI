#include "Compositor.h"

#include "Input.h"
#include "Log.h"

#include "../../src/MagelightCursorArt.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace mlcp::gfx {
namespace {
using Microsoft::WRL::ComPtr;

constexpr const char* kShader = R"(
cbuffer Rect : register(b0) { float4 rect; };
Texture2D page : register(t0);
SamplerState pointClamp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    float2 uv = float2(id & 1, id >> 1);
    V o;
    o.pos = float4(lerp(rect.x, rect.z, uv.x), lerp(rect.y, rect.w, uv.y), 0, 1);
    o.uv = uv;
    return o;
}
float4 ps(V i) : SV_Target { return page.Sample(pointClamp, i.uv); }
)";

constexpr UINT kSlots = 3;
constexpr UINT kMaxTextures = 64;     // SRV heap size; the cursor takes the last
constexpr int kCursorKey = -1;

struct Slot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> upload;
    UINT64 uploadSize = 0;
    UINT64 fenceValue = 0;
};

struct Texture {
    ComPtr<ID3D12Resource> res;
    UINT w = 0, h = 0, srv = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    bool hasContent = false;
};

struct Upload {
    Texture* tex;
    const std::uint8_t* data;
    UINT w, h, rowBytes;
};

struct State {
    bool ready = false, broken = false;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    std::vector<std::pair<HANDLE, UINT64>> armed;   // events a timed-out wait left armed, closed once passed
    UINT64 fenceValue = 0;
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3DBlob> vs, ps;
    std::map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> pso;
    ComPtr<ID3D12DescriptorHeap> srvHeap, rtvHeap;
    UINT srvStep = 0, nextSrv = 0;
    std::map<int, Texture> textures;
    Magelight::CursorArt::Image cursorArt;
    int cursorWant = 0;
    bool cursorUploaded = false;
    Slot slots[kSlots];
    UINT slot = 0;
    // Snapshots (EnableSnapshots).
    std::filesystem::path snapPath;
    std::atomic<bool> snapBusy{false};
    bool snapRecorded = false;
    UINT draws = 0;
    ComPtr<ID3D12Resource> readback;
    UINT64 readbackSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT snapFoot{};
    DXGI_FORMAT snapFormat = DXGI_FORMAT_UNKNOWN;
};
State& g = *new State;   // leaked: no D3D12 release from a static destructor at process exit

void Fail(const char* what, HRESULT hr) {
    Log("gfx: %s failed (0x%08lx), the pages are off for this session", what, static_cast<unsigned long>(hr));
    g.broken = true;
    input::Disable();   // nothing can be drawn: UI mode would take the game's input for a page nobody sees
}

ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(kShader, std::strlen(kShader), "mlcp", nullptr, nullptr, entry, target, 0, 0,
                                  &code, &errors);
    if (FAILED(hr)) {
        Log("gfx: shader %s: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

bool Init(ID3D12CommandQueue* queue) {
    HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&g.device));
    if (FAILED(hr)) return Fail("GetDevice", hr), false;
    for (auto& s : g.slots) {
        hr = g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator));
        if (FAILED(hr)) return Fail("CreateCommandAllocator", hr), false;
    }
    hr = g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.slots[0].allocator.Get(), nullptr,
                                     IID_PPV_ARGS(&g.list));
    if (FAILED(hr)) return Fail("CreateCommandList", hr), false;
    g.list->Close();
    hr = g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
    if (FAILED(hr)) return Fail("CreateFence", hr), false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kMaxTextures,
                                  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    hr = g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.srvHeap));
    if (FAILED(hr)) return Fail("CreateDescriptorHeap(srv)", hr), false;
    g.srvStep = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    hr = g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap));
    if (FAILED(hr)) return Fail("CreateDescriptorHeap(rtv)", hr), false;

    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 4};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, errors;
    hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(hr)) return Fail("D3D12SerializeRootSignature", hr), false;
    hr = g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.rootSig));
    if (FAILED(hr)) return Fail("CreateRootSignature", hr), false;

    g.vs = Compile("vs", "vs_5_0");
    g.ps = Compile("ps", "ps_5_0");
    if (!g.vs || !g.ps) return Fail("D3DCompile", E_FAIL), false;
    g.ready = true;
    Log("gfx: ready");
    return true;
}

ID3D12PipelineState* PsoFor(DXGI_FORMAT format) {
    if (auto it = g.pso.find(format); it != g.pso.end()) return it->second.Get();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = g.rootSig.Get();
    d.VS = {g.vs->GetBufferPointer(), g.vs->GetBufferSize()};
    d.PS = {g.ps->GetBufferPointer(), g.ps->GetBufferSize()};
    auto& rt = d.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;   // premultiplied alpha
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = format;
    d.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        Log("gfx: no pipeline for backbuffer format %d (0x%08lx)", static_cast<int>(format),
            static_cast<unsigned long>(hr));
        g.pso[format] = nullptr;
        return nullptr;
    }
    Log("gfx: pipeline for backbuffer format %d", static_cast<int>(format));
    g.pso[format] = pso;
    return pso.Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE CpuSrv(UINT i) {
    auto h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(i) * g.srvStep;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE GpuSrv(UINT i) {
    auto h = g.srvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(i) * g.srvStep;
    return h;
}

// A fresh event per wait: a timed-out wait leaves its event armed, and a shared one would let a later wait return
// while the GPU still reads what it guards.
bool WaitFence(UINT64 value, DWORD ms) {
    if (g.fence->GetCompletedValue() >= value) return true;
    HANDLE e = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!e) return false;
    if (FAILED(g.fence->SetEventOnCompletion(value, e))) return CloseHandle(e), false;
    const bool done = WaitForSingleObject(e, ms) == WAIT_OBJECT_0;
    if (done) CloseHandle(e);
    else g.armed.emplace_back(e, value);
    return done;
}

void CloseArmed() {
    if (g.armed.empty()) return;
    const UINT64 done = g.fence->GetCompletedValue();
    std::erase_if(g.armed, [done](const auto& a) { return a.second <= done && (CloseHandle(a.first), true); });
}

void WaitIdle() { WaitFence(g.fenceValue, INFINITE); }

// A texture of w x h for key, made or remade; its SRV slot stays the key's.
Texture* EnsureTexture(int key, UINT w, UINT h) {
    auto [it, made] = g.textures.try_emplace(key);
    Texture& t = it->second;
    if (made) {
        if (g.nextSrv >= kMaxTextures - 1 && key != kCursorKey) {
            Log("gfx: more than %u pages, view %d is not drawn", kMaxTextures - 1, key);
            g.textures.erase(it);
            return nullptr;
        }
        t.srv = key == kCursorKey ? kMaxTextures - 1 : g.nextSrv++;
    }
    if (t.res && t.w == w && t.h == h) return &t;
    if (t.res) WaitIdle();   // the old one may still be read
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    t.res.Reset();
    const HRESULT hr = g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&t.res));
    if (FAILED(hr)) return Fail("CreateCommittedResource(texture)", hr), nullptr;
    t.w = w;
    t.h = h;
    t.state = D3D12_RESOURCE_STATE_COPY_DEST;
    t.hasContent = false;
    g.device->CreateShaderResourceView(t.res.Get(), nullptr, CpuSrv(t.srv));
    return &t;
}

bool EnsureUpload(Slot& s, UINT64 size) {
    if (s.upload && s.uploadSize >= size) return true;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    s.upload.Reset();
    const HRESULT hr = g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&s.upload));
    if (FAILED(hr)) return Fail("CreateCommittedResource(upload)", hr), false;
    s.uploadSize = size;
    return true;
}

void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    g.list->ResourceBarrier(1, &b);
}

void SetState(Texture& t, D3D12_RESOURCE_STATES to) {
    if (t.state != to) Transition(t.res.Get(), t.state, to);
    t.state = to;
}

UINT Pitch(UINT w) {
    return (w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
}

UINT64 AlignPlacement(UINT64 v) {
    return (v + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
}

// Every changed page into this slot's one upload buffer, then a copy each. False when nothing could be recorded.
bool RecordUploads(Slot& s, const std::vector<Upload>& ups) {
    UINT64 total = 0;
    for (const auto& u : ups) total = AlignPlacement(total) + UINT64(Pitch(u.w)) * u.h;
    if (!total) return true;
    if (!EnsureUpload(s, total)) return false;
    void* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(s.upload->Map(0, &none, &mapped))) return false;
    UINT64 offset = 0;
    for (const auto& u : ups) {
        offset = AlignPlacement(offset);
        const UINT pitch = Pitch(u.w);
        for (UINT y = 0; y < u.h; ++y)
            std::memcpy(static_cast<std::uint8_t*>(mapped) + offset + UINT64(y) * pitch,
                        u.data + UINT64(y) * u.rowBytes, u.w * 4);
        SetState(*u.tex, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{u.tex->res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{s.upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint.Offset = offset;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, u.w, u.h, 1, pitch};
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        u.tex->hasContent = true;
        offset += UINT64(pitch) * u.h;
    }
    s.upload->Unmap(0, nullptr);
    return true;
}

void DrawQuad(Texture& t, int x, int y, float W, float H) {
    SetState(t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const float rect[4] = {2.f * x / W - 1.f, 1.f - 2.f * y / H, 2.f * (x + float(t.w)) / W - 1.f,
                           1.f - 2.f * (y + float(t.h)) / H};
    g.list->SetGraphicsRoot32BitConstants(0, 4, rect, 0);
    g.list->SetGraphicsRootDescriptorTable(1, GpuSrv(t.srv));
    g.list->DrawInstanced(4, 1, 0, 0);
}

bool RecordSnapshot(ID3D12Resource* back, const D3D12_RESOURCE_DESC& bd, UINT w, UINT h);

// The cursor's art, drawn at its pixel height for this screen; uploaded once.
const Magelight::CursorArt::Image& CursorArt(UINT screenH) {
    const int want = std::max(16, int(screenH / 1080.0 * 22));
    if (g.cursorWant != want) {
        g.cursorWant = want;
        g.cursorArt = Magelight::CursorArt::Arrow(want, 0);
        g.cursorUploaded = false;
    }
    return g.cursorArt;
}

// Snapshot region: everything drawn, plus a margin.
void Cover(UINT& w, UINT& h, int x, int y, UINT tw, UINT th, const D3D12_RESOURCE_DESC& bd) {
    w = std::max(w, std::min<UINT>(UINT(bd.Width), UINT(std::max(0, x)) + tw + 40));
    h = std::max(h, std::min<UINT>(bd.Height, UINT(std::max(0, y)) + th + 40));
}

void RecordDraws(IDXGISwapChain3* swapChain, const std::vector<views::Row>& rows, const input::Cursor& cursor) {
    ComPtr<ID3D12Resource> back;
    if (FAILED(swapChain->GetBuffer(swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back)))) return;
    const D3D12_RESOURCE_DESC bd = back->GetDesc();
    ID3D12PipelineState* pso = PsoFor(bd.Format);
    if (!pso) return;
    const auto rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(back.Get(), nullptr, rtv);

    for (const auto& r : rows) {   // every read texture to PIXEL_SHADER_RESOURCE before the render pass
        auto it = g.textures.find(r.id);
        if (r.visible && it != g.textures.end() && it->second.hasContent)
            SetState(it->second, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    Texture* cursorTex = nullptr;
    if (cursor.shown) {
        auto it = g.textures.find(kCursorKey);
        if (it != g.textures.end() && it->second.hasContent) {
            cursorTex = &it->second;
            SetState(*cursorTex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }
    Transition(back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const float W = float(bd.Width), H = float(bd.Height);
    const D3D12_VIEWPORT vp{0, 0, W, H, 0, 1};
    const D3D12_RECT sc{0, 0, LONG(bd.Width), LONG(bd.Height)};
    ID3D12DescriptorHeap* heaps[] = {g.srvHeap.Get()};
    g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g.list->RSSetViewports(1, &vp);
    g.list->RSSetScissorRects(1, &sc);
    g.list->SetPipelineState(pso);
    g.list->SetGraphicsRootSignature(g.rootSig.Get());
    g.list->SetDescriptorHeaps(1, heaps);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    UINT coverW = 0, coverH = 0;
    for (const auto& r : rows) {
        auto it = g.textures.find(r.id);
        if (!r.visible || it == g.textures.end() || !it->second.hasContent) continue;
        DrawQuad(it->second, r.x, r.y, W, H);
        Cover(coverW, coverH, r.x, r.y, it->second.w, it->second.h, bd);
    }
    if (cursorTex) {
        const int cx = cursor.x - int(g.cursorArt.hotX), cy = cursor.y - int(g.cursorArt.hotY);
        DrawQuad(*cursorTex, cx, cy, W, H);
        Cover(coverW, coverH, cx, cy, cursorTex->w, cursorTex->h, bd);
    }

    if (!g.snapPath.empty() && coverW && ++g.draws % 600 == 0 && !g.snapBusy &&
        RecordSnapshot(back.Get(), bd, coverW, coverH))
        return;
    Transition(back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
}

// Ends with the backbuffer in PRESENT, as RecordDraws would.
bool RecordSnapshot(ID3D12Resource* back, const D3D12_RESOURCE_DESC& bd, UINT w, UINT h) {
    switch (bd.Format) {   // the BMP writer reads 8-bit BGRA or RGBA; an HDR backbuffer is not shot
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        break;
    default: {
        static bool told = false;
        if (!told) Log("gfx: no snapshots of a backbuffer in format %d (8-bit only)", int(bd.Format));
        told = true;
        return false;
    }
    }
    D3D12_RESOURCE_DESC region = bd;
    region.Width = w;
    region.Height = h;
    UINT64 size = 0;
    g.device->GetCopyableFootprints(&region, 0, 1, 0, &g.snapFoot, nullptr, nullptr, &size);
    if (!g.readback || g.readbackSize < size) {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = size;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        g.readback.Reset();
        if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&g.readback))))
            return false;
        g.readbackSize = size;
    }
    Transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{back, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{g.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = g.snapFoot;
    const D3D12_BOX box{0, 0, 0, w, h, 1};
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    Transition(back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    g.snapFormat = bd.Format;
    g.snapRecorded = true;
    return true;
}

// Off the presenting thread: wait for the copy, write a top-down 32-bit BMP.
void WriteSnapshot(UINT64 fenceValue) {
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.fence->SetEventOnCompletion(fenceValue, ev);
    WaitForSingleObject(ev, 5000);
    CloseHandle(ev);
    const auto& f = g.snapFoot.Footprint;
    void* mapped = nullptr;
    D3D12_RANGE all{0, SIZE_T(g.readbackSize)};
    if (SUCCEEDED(g.readback->Map(0, &all, &mapped))) {
        std::vector<std::uint8_t> px(std::size_t(f.Width) * f.Height * 4);
        const bool rgba = g.snapFormat == DXGI_FORMAT_R8G8B8A8_UNORM || g.snapFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        for (UINT row = 0; row < f.Height; ++row) {
            const auto* s = static_cast<const std::uint8_t*>(mapped) + std::size_t(row) * f.RowPitch;
            auto* d = px.data() + std::size_t(row) * f.Width * 4;
            for (UINT i = 0; i < f.Width; ++i) {
                d[i * 4 + 0] = s[i * 4 + (rgba ? 2 : 0)];
                d[i * 4 + 1] = s[i * 4 + 1];
                d[i * 4 + 2] = s[i * 4 + (rgba ? 0 : 2)];
                d[i * 4 + 3] = 255;
            }
        }
        D3D12_RANGE none{0, 0};
        g.readback->Unmap(0, &none);
        BITMAPFILEHEADER fh{0x4d42, DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + px.size()), 0, 0,
                            sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
        BITMAPINFOHEADER ih{sizeof ih, LONG(f.Width), -LONG(f.Height), 1, 32, BI_RGB};
        if (FILE* out = _wfopen(g.snapPath.c_str(), L"wb")) {
            fwrite(&fh, sizeof fh, 1, out);
            fwrite(&ih, sizeof ih, 1, out);
            fwrite(px.data(), 1, px.size(), out);
            fclose(out);
            Log("gfx: snapshot %ux%u written (backbuffer format %d)", f.Width, f.Height, int(g.snapFormat));
        }
    }
    g.snapBusy = false;
}
}

void EnableSnapshots(const std::filesystem::path& bmp) {
    g.snapPath = bmp;
    Log("gfx: snapshots on, every 600 draws to %ls", bmp.c_str());
}

bool Draw(IDXGISwapChain3* swapChain, ID3D12CommandQueue* queue, const std::vector<views::Row>& rows,
          const std::map<int, views::Pixels>& changed, const input::Cursor& cursor) {
    if (g.broken || !swapChain || !queue) return true;
    if (!g.ready && !Init(queue)) return true;
    CloseArmed();

    // The slot first: a frame skipped here must not have used up what it was handed.
    Slot& s = g.slots[g.slot];
    if (!WaitFence(s.fenceValue, 100)) return false;   // skip a frame, never hang the game

    std::vector<Upload> ups;
    for (const auto& [id, px] : changed) {
        if (!px.data || !px.width || !px.height) continue;
        if (Texture* t = EnsureTexture(id, px.width, px.height)) ups.push_back({t, px.data, px.width, px.height, px.rowBytes});
        if (g.broken) return true;
    }
    bool cursorUp = false;
    if (cursor.shown) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        swapChain->GetDesc1(&d);
        const auto& art = CursorArt(d.Height);
        if (!g.cursorUploaded) {
            if (Texture* t = EnsureTexture(kCursorKey, UINT(art.w), UINT(art.h))) {
                ups.push_back({t, reinterpret_cast<const std::uint8_t*>(art.px.data()), UINT(art.w), UINT(art.h),
                               UINT(art.w) * 4});
                cursorUp = true;
            }
        }
    }
    bool anything = !ups.empty() || cursor.shown;
    for (const auto& r : rows)
        if (r.visible && g.textures.count(r.id) && g.textures[r.id].hasContent) anything = true;
    if (!anything) return true;

    if (FAILED(s.allocator->Reset()) || FAILED(g.list->Reset(s.allocator.Get(), nullptr))) return false;
    if (!RecordUploads(s, ups)) {
        g.list->Close();
        return false;
    }
    if (!g.broken) RecordDraws(swapChain, rows, cursor);

    if (FAILED(g.list->Close())) return Fail("Close", E_FAIL), true;
    if (cursorUp) g.cursorUploaded = true;
    ID3D12CommandList* lists[] = {g.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(g.fence.Get(), ++g.fenceValue);
    s.fenceValue = g.fenceValue;
    if (g.snapRecorded) {
        g.snapRecorded = false;
        g.snapBusy = true;
        std::thread(WriteSnapshot, g.fenceValue).detach();
    }
    g.slot = (g.slot + 1) % kSlots;
    return true;
}
}
