# Builds MagelightCP into $Out (default C:\b\mlcp); the stage is $Out\stage, laid out as the game root.
param(
    [Parameter(Mandatory)] [string] $Red4extSdk,
    [string] $Out = $(if ($env:MLCP_BUILD_DIR) { $env:MLCP_BUILD_DIR } else { "C:\b\mlcp" })
)
$ErrorActionPreference = "Stop"

$json = Join-Path $PSScriptRoot "extern\nlohmann\include\nlohmann\json.hpp"
if (-not (Test-Path $json)) {
    New-Item -ItemType Directory -Force (Split-Path $json) | Out-Null
    Invoke-WebRequest "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp" -OutFile $json
}

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
cmd /c "`"$(Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat')`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}

cmake -G Ninja -S $PSScriptRoot -B $Out -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DRED4EXT_SDK_DIR=$Red4extSdk"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $Out -j 4
exit $LASTEXITCODE
