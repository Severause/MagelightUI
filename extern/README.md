# extern/

## ultralight/ (not committed)

The Ultralight SDK, build of record **2.0.0-beta.2 (Free)**: `VERSION.txt` inside the SDK must
read `2.0.0-beta.2`, and `build.ps1` refuses any other build, so moving to another one is a
deliberate edit of that pin (see "The Ultralight SDK pin" in [CONTRIBUTING.md](../CONTRIBUTING.md)).
Extract the Windows x64 Free SDK (`ul-sdk-2.0.0-beta.2-win-x64-free.7z` from https://ultralig.ht/download,
SHA-256 `e17b3ca2f3c2063f1d14971e7ef756f28e71f2d426a91d2e21c5236d2dc67e6d`) so that
`extern/ultralight/VERSION.txt` exists, or make `extern/ultralight` a junction to an extracted copy.
Once Ultralight stops offering this build, the archive kept with the release records
(RELEASING.md section 2) is the way to it.

(No 7-Zip installed? Windows 11's own bsdtar reads 7z; unlike 7-Zip's `-o`,
its `-C` needs the folder to exist:
`mkdir extern\ultralight` then
`C:\Windows\System32\tar.exe -xf <archive>.7z -C extern\ultralight`.)

Keep an archive inside `extern/`: `.gitignore` covers `extern/*.7z`, not a
`.7z` at the repository root. Delete it once extracted.

What the build uses:

- `include/`, `lib/`, `bin/`, `resources/`, `license/` and `inspector/`: the runtime, its import
  libraries, its data, its licence texts and the Web Inspector. `bin/` holds four DLLs; three ship
  (renamed, see `build.ps1`). `AppCore.dll` does not: 2.0's imports CoreMessaging.dll and the WinRT
  api-sets, and Magelight needs nothing from it but the font loader.
- `platform/`: Ultralight's reference platform implementations as source, under the zlib licence
  (`platform/LICENSE`). Two parts are built, in place and never copied here:
  `platform/src/fonts/FontLoaderWin.cpp` and `TextAnalysisSource.cpp` are compiled unmodified
  into `Magelight.dll`, and `gpu/` adapts the Direct3D 11 reference driver (`gpu/README.md`).
- `platform/shaders/generated/headers/d3d11`: the compiled shaders `MagelightGPU.dll` embeds,
  read through an include path. They are NOT zlib: proprietary to Ultralight, Inc. and licensed
  only for use with Ultralight (`platform/shaders/generated/LICENSE`), so they are never copied
  into this repository.

Nothing else of `platform/` is built: `src/filesystem/FileSystemWin.cpp` is BSD, `src/fonts/FallbackFont.h`
is Apache-2.0 (Roboto), and `src/Factories.cpp` would pull in the platform's file system, clipboard,
logger and GPU contexts. `samples/` and `tools/` are not used.

## appcore-ref/ (committed)

Verbatim AppCore D3D11 driver sources (LGPL-2.1, its own `LICENSE`) that the Ultralight 1.4
`gpu/MagelightGpuDriver.cpp` was adapted from. The 2.0 driver is rebased on the SDK's zlib
reference driver and contains none of it; this folder is history and builds nothing.
