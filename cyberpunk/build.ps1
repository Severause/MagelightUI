# Builds MagelightCP into $Out (default C:\b\mlcp); the stage is $Out\stage, laid out as the game root.
# Libraries come from vcpkg (vcpkg.json), as the root build.ps1 gets them: standalone vcpkg at VCPKG_ROOT, else C:\vcpkg.
param(
    [string] $Out = $(if ($env:MLCP_BUILD_DIR) { $env:MLCP_BUILD_DIR } else { "C:\b\mlcp" })
)
$ErrorActionPreference = "Stop"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = if (Test-Path $vswhere) {
    & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsPath) {
    $vsPath = @("C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools",
                "C:\Program Files\Microsoft Visual Studio\18\Community",
                "C:\Program Files\Microsoft Visual Studio\2022\Community") |
        Where-Object { Test-Path (Join-Path $_ "VC\Auxiliary\Build\vcvars64.bat") } | Select-Object -First 1
}
if (-not $vsPath) { throw "No Visual Studio with C++ tools found" }

# The caller's VCPKG_ROOT is read before vcvars, which sets it to the copy Visual Studio bundles (never used).
$vcpkgRoot = $env:VCPKG_ROOT
if ($vcpkgRoot -and ($vcpkgRoot.TrimEnd('\') + '\').StartsWith($vsPath.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    $vcpkgRoot = $null
}
if (-not $vcpkgRoot) { $vcpkgRoot = "C:\vcpkg" }
$toolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $toolchain)) {
    throw "vcpkg toolchain not found at $toolchain - install standalone vcpkg (CONTRIBUTING.md step 2) and set VCPKG_ROOT"
}

cmd /c "`"$(Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat')`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
$env:VCPKG_ROOT = $vcpkgRoot
$env:VCPKG_VISUAL_STUDIO_PATH = $vsPath

cmake -G Ninja -S $PSScriptRoot -B $Out -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $Out -j 4
exit $LASTEXITCODE
