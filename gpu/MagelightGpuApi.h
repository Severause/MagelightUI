#pragma once
// Magelight UI — public C ABI of the GPU backend DLL (MagelightGPU.dll).
//
// This header is part of the HOST project: it only DESCRIBES the boundary. The
// backend behind it is adapted from the Ultralight SDK's reference D3D11 driver
// (Zlib) and lives in its own dynamically-loaded DLL, so the host can run without
// it (CPU surfaces) and the backend can be rebuilt on its own — see gpu/README.md.
//
// The host resolves these via GetProcAddress after preloading the (namespaced)
// Ultralight runtime; MagelightGPU.dll's own imports bind against the modules
// already in the process.

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

#ifdef MGGPU_BUILD
#  define MGGPU_API __declspec(dllexport)
#else
#  define MGGPU_API
#endif

// The C ABI's revision: bump it whenever an MgGpu_* signature or meaning changes. The host refuses a backend built
// with another revision. A backend exports every function below; the host treats a missing one as a broken build.
#define MGGPU_CONTRACT 2u

extern "C" {

    // What a backend was built with, for the host's version check (MgGpu_GetInfo). A backend built for another
    // Ultralight SDK reads every command at the wrong offsets, and a crash inside a frame is the first sign, so the
    // host compares the command sizes too, not only the version text. Fields may be appended, never reordered:
    // `size` tells the host how much of the struct the backend filled.
    struct MgGpuInfo {
        std::uint32_t size;               // sizeof(MgGpuInfo) in the backend's build
        std::uint32_t contract;           // MGGPU_CONTRACT in the backend's build
        std::uint32_t gpuStateSize;       // sizeof(ultralight::GPUState) in the backend's build
        std::uint32_t commandSize;        // sizeof(ultralight::Command) in the backend's build
        const char*   ultralightVersion;  // ULTRALIGHT_VERSION of the SDK headers it was built against
    };
    // Static data, valid for the DLL's lifetime; any thread. A backend without this export predates the check.
    MGGPU_API const MgGpuInfo* MgGpu_GetInfo();

    // Create a driver bound to the GAME's device/context (not owned). `log`
    // receives human-readable errors (may be null). Returns an opaque handle,
    // or null on failure.
    typedef void (*MgGpuLogFn)(const char* message);
    MGGPU_API void* MgGpu_Create(ID3D11Device* device, ID3D11DeviceContext* context, MgGpuLogFn log);

    MGGPU_API void MgGpu_Destroy(void* handle);

    // The ultralight::GPUDriver* to hand to Platform::set_gpu_driver. Valid
    // for the handle's lifetime. (A C++ interface pointer is safe across this
    // boundary: both DLLs are built together with the same toolchain and link
    // the same Ultralight module.)
    MGGPU_API void* MgGpu_GetGPUDriver(void* handle);

    // Frame execution: after Renderer::Render() has buffered commands.
    MGGPU_API int  MgGpu_HasCommandsPending(void* handle);
    MGGPU_API void MgGpu_DrawCommandList(void* handle);

    // MSAA sample count (1, 2, 4 or 8) for antialiased render targets created
    // from now on; a count the device cannot do steps down. Returns the count in
    // effect, 0 for a null handle. Render thread. Call it BEFORE handing the
    // driver to Platform::set_gpu_driver: the engine reads the driver's MSAA
    // capability once (GetDeviceCaps), so a count raised after that read is
    // clamped to what the engine was told (1 = MSAA was reported unsupported).
    MGGPU_API int MgGpu_SetSampleCount(void* handle, int samples);

    // ID3D11ShaderResourceView* for a driver texture id (a View's
    // render_target().texture_id) — what the host's compositor samples.
    // Null if unknown. Not a plain getter: with MSAA on, this call records
    // the resolve on the immediate context, so its contents are current only
    // as of this call. Render thread only, after DrawCommandList, once per
    // frame whose pixels you read; the pointer is stable for the texture's
    // life, but never reuse it across frames without calling again (a cached
    // or before-draw read shows a stale frame).
    MGGPU_API void* MgGpu_GetTextureSRV(void* handle, std::uint32_t texture_id);

    // ── External textures (Ultralight ImageSource) ──────────────────────
    // Register an SRV the HOST (or an embedding mod) owns under a fresh
    // driver texture id — the id an ImageSource::CreateFromTexture refers
    // to. Ultralight never uploads, resizes or destroys it; its FillType_
    // Image draws simply bind this SRV. The driver AddRef's the SRV. All
    // three run on the render thread only (the driver's texture map is not
    // locked). Returns 0 on failure.
    MGGPU_API std::uint32_t MgGpu_RegisterExternalTexture(void* handle, ID3D11ShaderResourceView* srv);
    // Re-point an external id at a new SRV (resize/recreate). 1 = ok.
    MGGPU_API int MgGpu_SetExternalTextureSRV(void* handle, std::uint32_t texture_id, ID3D11ShaderResourceView* srv);
    // Drop the driver entry. Only safe once no page can still reference the id.
    MGGPU_API void MgGpu_UnregisterExternalTexture(void* handle, std::uint32_t texture_id);

    // ── What the engine asked for (contract 2) ──────────────────────────
    // Counts kept by the backend. The host's memory line reads renderTargets, msaaSurfaces, msaaBytes and
    // antialiasedCreated: whether the engine asked for antialiased targets, which Ultralight's memory report does not
    // say (its render-target bytes include any multisample surface). The other fields serve diagnostics and offline
    // probes. Bytes are what the backend allocated. Fields may be appended, never reordered: the caller sets `size` to
    // sizeof(MgGpuStats) of its build, and the backend fills at most that much and sets `size` to what it filled.
    struct MgGpuStats {
        std::uint32_t size;
        std::uint32_t renderTargets;         // render-target textures alive now
        std::uint32_t antialiasedTargets;    // of those, created with kGPUTextureFlag_Antialiased
        std::uint32_t msaaSurfaces;          // of those, drawn into a multisample surface (sample count above 1)
        std::uint64_t targetsCreated;        // render-target textures created since MgGpu_Create
        std::uint64_t antialiasedCreated;    // of those, created with kGPUTextureFlag_Antialiased
        std::uint64_t textureBytes;          // live textures that are not render targets (external ones excluded)
        std::uint64_t targetBytes;           // live render-target textures, single-sample
        std::uint64_t msaaBytes;             // live multisample surfaces
        std::uint64_t flushes;               // CommandType::Flush commands executed since MgGpu_Create
    };
    // 1 when filled, 0 for a null handle or `out`. Render thread (the counters are the driver's, unlocked).
    MGGPU_API int MgGpu_GetStats(void* handle, MgGpuStats* out);

}  // extern "C"
