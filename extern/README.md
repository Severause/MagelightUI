# extern/

## ultralight/ (not committed)

The Ultralight **1.4** SDK. The release CDN's `latest` alias still serves
1.3.0 (checked 2026-09-01), so 1.4 comes from the dev channel — the build of
record is `1.4.0b.081c48b` (see `VERSION.txt` inside the SDK; the dev CDN
tracks master, so a re-fetch may yield a newer build; `build.ps1` and CI
refuse any build but the pinned one, so moving to a new build is a deliberate
edit of that pin). From the repository root:

    curl -L -o extern/ultralight-sdk-dev.7z https://ultralight-sdk-dev.sfo2.cdn.digitaloceanspaces.com/ultralight-sdk-latest-win-x64.7z
    7z x -oextern/ultralight extern/ultralight-sdk-dev.7z

(No 7-Zip installed? Windows 11's own bsdtar reads 7z; unlike 7-Zip's `-o`,
its `-C` needs the folder to exist:
`mkdir extern\ultralight` then
`C:\Windows\System32\tar.exe -xf extern\ultralight-sdk-dev.7z -C extern\ultralight`.)

Keep the archive inside `extern/`: `.gitignore` covers `extern/*.7z`, not a
`.7z` at the repository root. Delete it once extracted.

Then check `extern/ultralight/VERSION.txt`. If it is newer than the pin, the dev
CDN has moved on and no longer serves the build of record: see "The Ultralight
SDK pin" in [CONTRIBUTING.md](../CONTRIBUTING.md).

Layout expected by CMakeLists.txt: `ultralight/include`, `ultralight/lib`,
`ultralight/bin`, `ultralight/resources`. The 1.4 SDK also ships top-level
`shaders/` (the fxc-compiled HLSL binaries `gpu/shaders/` mirrors) and
`samples/`, which the build doesn't consume.

## appcore-ref/ (committed)

Verbatim upstream AppCore D3D11 driver sources (LGPL-2.1) that
`gpu/MagelightGpuDriver.cpp` was adapted from — kept as provenance for the
LGPL corresponding-source obligation. See `gpu/README.md`.
