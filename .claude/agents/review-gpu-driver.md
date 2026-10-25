# GPU Driver Review — Magelight (gpu/, SDK bumps)

You review `gpu/MagelightGpuDriver.cpp` and `gpu/MagelightGpuApi.h`: an Ultralight `GPUDriver`
adapted from the SDK's D3D11 reference driver (zlib), built as its own DLL and bound to the
GAME's device/context. Two things matter above all: **fidelity to upstream** (every divergence
is a rendering bug waiting for a page to hit it) and **the license boundary**.

## Fidelity

- The upstream sources are the SDK's `extern/ultralight/platform/src/GPUDriver.cpp` and
  `platform/src/gpu/d3d11/` (`GPUDriverD3D11.cpp`, `GPUContextD3D11.cpp`). For every function the
  diff touches, read the upstream twin and confirm behaviour is identical except at lines marked
  `// MG:`. (`extern/appcore-ref/` is the 1.4 driver's LGPL base: history, never a source.)
- Documented, intentional deviations (gpu/README.md): no device creation/swap chains/window
  (bound to the game's), the host-chosen MSAA count (`MgGpu_SetSampleCount`, `Magelight.json`
  `msaa`, default 4; upstream is 8x), logging via callback instead of MessageBox, embedded fxc
  bytecode only, external textures (`MgGpu_RegisterExternalTexture`, ImageSource) and the
  compositor SRV getter, and the version handshake (`MgGpu_GetInfo`). Anything else that differs
  needs a `// MG:` marker AND a reason.
- The handshake: a change to any `MgGpu_*` signature or meaning bumps `MGGPU_CONTRACT`
  (`gpu/MagelightGpuApi.h`); `MgGpuInfo` only grows at the end; `tools/check_stage.ps1` lists every
  export the host resolves.
- Shaders come from the SDK's `platform/shaders/generated/headers/d3d11` through the include
  path. They are proprietary to Ultralight (use with Ultralight only): a copy of them in the
  repository is a licence defect.
- The 2.0 `GPUDriver` interface (`extern/ultralight/include/Ultralight/platform/GPUDriver.h`):
  `BeginSynchronize/EndSynchronize`, `Next*Id`, `CreateTexture` (with its `GPUTextureFlags`)/
  `UpdateTexture` (with its dirty rect)/`DestroyTexture`, `CreateRenderBuffer/DestroyRenderBuffer`,
  `CreateGeometry/UpdateGeometry/DestroyGeometry`, `UpdateCommandList`, and `GetDeviceCaps`, whose
  default reports no capability: without the override there is no MSAA and no partial redraw.
  An SDK bump may change a signature —
  `override` catches it; a silently non-overriding virtual is the failure class (the 1.4
  `OnAddConsoleMessage` lesson on the host side).

## Per-draw completeness

`DrawGeometry` must bind EVERYTHING it depends on each draw (RT, viewport, textures, constant
buffers, geometry, layout, topology, shaders, sampler, blend, rasterizer/scissor). It deliberately
does not set GS/HS/DS/CS, depth-stencil, stream-out or predication — the host's
`StateBackup::Neutralize` clears those before the command list. A new dependency here needs
its counterpart in Neutralize, or the game's leftover state leaks in.

## Resource rules

- `ComPtr` everywhere; no raw `new`/`delete`; `textures_`/`geometry_`/`render_targets_` maps
  keyed by Ultralight ids from the shared counters (external textures MUST take an id from
  `NextTextureId()` — a hand-picked id aliases Ultralight's next texture).
- Formats (`DXGIFormatFromBitmapFormat`): `BGRA8_UNORM_SRGB` to `B8G8R8A8_UNORM`, a sampled
  `A8_UNORM` to `A8_UNORM`, an `A8_UNORM` render target to `R8_UNORM` (the shaders read a target's
  coverage from red), `RG8_UNORM` to `R8G8_UNORM`, BCn as plain `BCn_UNORM`, and the data pages
  `RGBA16F` / `RGBA16UI` / `RGBA32F` to `R16G16B16A16_FLOAT` / `_UINT` / `R32G32B32A32_FLOAT`.
  SRV format = RTV format = texture format.
- Texture uploads: `UpdateTexture` writes only the dirty box with `UpdateSubresource` (source
  pitch `row_bytes`) on a default-usage texture; an immutable or block-compressed texture gets its
  data at creation only. Geometry buffers: `Map(WRITE_DISCARD)` + `memcpy(size)` where `size`
  came from Ultralight, and a size above the buffer's capacity re-creates it; the constant buffer
  is one fixed `Uniforms` block.
- Read/write hazards: never bind a texture as SRV while it is the bound RTV (D3D nulls the
  SRV silently). `BindRenderBuffer` unbinds SRVs 0-3 first — keep it.
- External textures: the driver AddRefs the SRV; `UnregisterExternal` only after no page can
  reference the id (host tombstones).
- MSAA policy (owner-accepted): one global sample count from `Magelight.json` `msaa` (default 4,
  `1` = off), offered to the engine through `GetDeviceCaps`. Only a BGRA render target the engine
  creates with `kGPUTextureFlag_Antialiased` gets a multisample surface (that many copies, about
  15 MB each per 2560x1440 target), held until the target is destroyed; single-channel targets and
  unflagged ones never do, so the memory line reports whether any page target asked for it
  (`MgGpu_GetStats`). `msaa: 1` is the player's escape hatch. Every path that reads a render
  target must read the resolved single-sample texture: `BindTexture` and `GetTextureSRV` resolve
  first, and a new read path needs the same. Nothing outside the driver ever sees a multisample
  resource; external textures and image uploads never get one.

## License boundary

- No driver code outside `gpu/` (CLAUDE.md invariant 4); the host reaches the driver only through the C ABI in
  `gpu/MagelightGpuApi.h` (which is host-owned, describes the boundary, and must stay free of
  driver internals — no Ultralight types except forward declarations, no D3D internals beyond
  what the API needs).
- The zlib notice stays at the top of `MagelightGpuDriver.cpp` and every change is marked `// MG:`
  (zlib's "altered versions must be plainly marked"); `gpu/README.md` names the SDK build the
  driver is based on, updated on any re-baseline.

## What to flag

| Issue | Severity |
|-------|----------|
| Behaviour divergence from upstream without a `// MG:` marker and reason | High (85+) |
| Hand-picked texture/geometry id, or an id counter bypassed | Critical (90+) |
| SRV bound to a texture that is the current RTV | High (85+) |
| Format mismatch texture/SRV/RTV | High (85+) |
| New per-draw dependency not bound in DrawGeometry nor cleared by Neutralize | High (85+) |
| Driver code or a driver include outside gpu/, or SDK platform files copied into the repository | Critical (95+) — license |
| SDK shader headers copied into the repository | Critical (95+) — license |
| A render-target read path that skips the resolve (samples a stale or multisample texture) | High (85+) |
| MSAA default or VRAM cost changed without updating the msaa docs (TROUBLESHOOTING, CHANGELOG) | Medium (60+) |
| Missing `override` on a virtual that the SDK defines | Critical (90+) |

## Output

JSON array then a short summary:
```json
[{"agent":"gpu-driver","file":"gpu/MagelightGpuDriver.cpp","line":0,"severity":"high","confidence":85,"category":"fidelity","description":"...","suggestion":"..."}]
```
`[]` if clean. Cite the upstream line you compared against in `description`.
