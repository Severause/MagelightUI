# gpu/ — the Magelight GPU backend (MagelightGPU.dll)

**License: Zlib** (the notice is reproduced at the top of `MagelightGpuDriver.cpp`). This component is
adapted from the Direct3D 11 reference GPUDriver that ships as source in the **Ultralight 2.0.0-beta.2 SDK**
(`platform/src/GPUDriver.cpp`, `platform/src/gpu/d3d11/GPUDriverD3D11.cpp`,
`platform/src/gpu/d3d11/GPUContextD3D11.cpp`; Copyright (c) 2026 Ultralight, Inc.). It is an altered version
of that source: the changes are marked `// MG:` and listed below.

**Licence.** `LICENSE` holds the Zlib text this folder is under. The 2.0 port carries no AppCore code (the 1.4
driver was adapted from AppCore's LGPL-2.1 driver; `extern/appcore-ref/`, with its own `LICENSE`, is that historical
base and builds nothing), so no LGPL corresponding-source obligation applies to this DLL. Zlib asks only that the
notice stays in the source and that altered versions are marked as such; `NOTICES.txt` and
`THIRD_PARTY_LICENSES.txt` credit the SDK for the shipped DLL.

**Shaders.** The compiled D3D11 shader bytecode is NOT in this folder any more. The build reads it from the
SDK's `platform/shaders/generated/headers/d3d11` through an include path (see the MagelightGPU section of
`CMakeLists.txt`). Those headers are proprietary to Ultralight, Inc. and licensed only for use with the
Ultralight library: never copy them into this repository. The built DLL embeds them, which is use with
Ultralight.

The backend stays its OWN dynamically-loaded DLL: the host (`Magelight.dll`) talks to it only through the C ABI
in `MagelightGpuApi.h`, loads it at runtime, and falls back to the CPU surface path when it is absent.

Modifications vs the SDK reference driver (marked `// MG:` in source):
- The driver binds to the GAME's existing device and immediate context (no device creation, no swap chains, no
  window), and creating it touches no pipeline state. Skyrim owns the device; we render offscreen.
- **The game's pipeline state is saved and restored around every command list** (`PipelineBackup`): IA
  (topology, input layout, VB 0, IB), VS and PS (shader with class instances, CB0 with its 11.1 offsets, SRVs
  t0-t3), the PS sampler, GS/HS/DS, OM (blend state, factor and sample mask, depth-stencil state and ref, all 8
  RTVs + DSV, and every UAV the one-target `OMSetRenderTargets` unbinds), RS (state, every viewport and scissor
  rect), stream-output targets and predication. GS/HS/DS, depth-stencil, stream output and predication are
  neutralized for the list's draws. `CreateGeometry` no longer binds an input layout (the SDK does, outside any
  backup). The texture upload (`UpdateSubresource`) and the host's MSAA resolve run with the game's predicate
  lifted (`ScopedNoPredication`): both are predicated in D3D11 and run outside the list.
- MSAA is the host's choice (`MgGpu_SetSampleCount`, from Magelight.json `msaa`), where the SDK fixes 8x. The
  engine is told the count through `GetDeviceCaps` and asks for it per target (`kGPUTextureFlag_Antialiased`); an
  antialiased BGRA target draws into its own multisample surface and resolves into the plain texture everything
  samples, so the host's SRV and the engine's binds never see a multisample resource.
- A draw whose source texture is the texture behind the bound render target leaves that slot null (the SDK does
  the same now; with MSAA, binding it would also resolve the target mid-layer and drop the layer's last draws).
- The capabilities are measured on the game's device: a compressed family (BC1-BC3, BC7) is reported only when
  every format of it samples as a Texture2D; the largest texture size follows the feature level; partial redraw
  is reported (render targets persist on a desktop D3D11 device).
- Errors go to a host-provided log callback, once per distinct message (the SDK's `LogErrorOnce`), with the
  device-removed reason when the device is gone; no MessageBox.
- A draw whose render target, geometry or shader program is missing is skipped instead of drawing into whatever
  is bound (at the start of a list, the game's target). `UpdateGeometry` checks its `Map` and reallocates a
  buffer an update outgrows. A failed blend state is cached as null (D3D11's default, blending off).
- External textures (`MgGpu_RegisterExternalTexture` and friends): an SRV the host owns, entered under an id from
  the same counter the engine's `CreateTexture` uses; `UpdateTexture` refuses it.
- `MgGpu_GetInfo` reports what the DLL was built with: the C ABI revision (`MGGPU_CONTRACT` in
  `MagelightGpuApi.h`), `sizeof(GPUState)`, `sizeof(Command)` and the SDK's `ULTRALIGHT_VERSION`. The host refuses
  a DLL whose values differ from its own. Bump `MGGPU_CONTRACT` whenever an `MgGpu_*` signature or meaning
  changes; append to `MgGpuInfo`, never reorder it.
- The six quad-layout programs share one vertex shader object; shaders always load from the embedded bytecode.
- `MgGpu_GetStats` (contract 2) counts what the engine asked for: live render targets, how many were created with
  `kGPUTextureFlag_Antialiased` and how many hold a multisample surface, the bytes allocated (multisample surfaces
  apart) and the `Flush` commands run. The host's memory line reads it to say whether any page target took MSAA,
  which Ultralight's own memory report does not (its render-target bytes include the multisample surfaces).
- Testing-only parts of the SDK driver (readback, GPU timing, its memory stats) are left out.
- Contract 3 (1.7.0): the context `MgGpu_Create` is given may be a **deferred context** of the game's device (the
  host's page thread, `"pageThread"`). Everything the driver does is then recorded and the host runs the list on the
  immediate context at Present. On a deferred context `Flush` does nothing (a list cannot record one), the pipeline
  backup and predication scopes are skipped (a list starts from default state and holds nothing of the game's), and
  a boxed `UpdateSubresource` is given the bitmap's origin where the runtime emulates command lists
  (`DriverCommandLists` false), which otherwise applies the box's offset twice. `tools/deferred-test` checks it
  against the immediate context byte for byte, on hardware and on WARP (emulated lists).
