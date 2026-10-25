# check_stage.ps1 — release/stage lint for Magelight (run after build.ps1).
#
# Checks (each guards a failure class that has bitten or would ship silently):
#   1. Namespaced runtime: the stage carries the renamed DLLs, NONE of Ultralight's original names, and no
#      MgACore.dll (AppCore is compiled out: Magelight.dll builds in the SDK's font loader instead).
#   2. No original module-name strings survive INSIDE any staged DLL, ASCII or UTF-16 (the same-length byte
#      patch must have reached every import; a leftover 'Ultralight.dll' rebinds to another host's copy), and
#      none names MgACore.dll (an import or LoadLibrary of it would fail on every install).
#   3. Required stage files exist (host DLL, driver, resources, focus-menu SWF, licence texts).
#   4. CMake project VERSION parses and matches the built DLL's PLUGIN_VERSION string.
#   4b. MagelightGPU.dll exports every MgGpu_* function the host resolves (the optional ones too: a stage
#      built from a stale driver would silently lose MSAA or images, since the host treats them as absent).
#   5. (optional) api/MagelightUI_API.h is byte-identical to SeverActions' vendored copy.
#   6. The host DLL loads alone, with the runtime out of reach (every runtime import is delay-loaded and SKSE loads
#      the plugin before PreloadRuntime can run): a static whose constructor calls into Ultralight fails it. A
#      process that loaded it that way also exits cleanly: a static whose destructor does fails that. And a view
#      registers there (v1 CreateView), as consumers do on a host whose runtime did not load: a view record that
#      reaches the runtime when it is built (an SDK struct's imported constructor) fails that.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools/check_stage.ps1 [-Stage <dir>] [-SaHeader <path>] [-NoPex]
#   -Stage defaults to build.ps1's: $env:MG_BUILD_DIR\stage when MG_BUILD_DIR is set, else C:\b\mgl\stage.
#   -NoPex: skip the Magelight.pex requirement — for CI runners, which have no
#   Papyrus compiler (it ships with the game). A release zip must pass WITHOUT it.
param(
    [string]$Stage = $(if ($env:MG_BUILD_DIR) { Join-Path $env:MG_BUILD_DIR 'stage' } else { 'C:\b\mgl\stage' }),
    [string]$SaHeader = "",
    [switch]$NoPex
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$fail = 0
function Fail($msg) { Write-Host "FAIL  $msg" -ForegroundColor Red; $script:fail++ }
function Pass($msg) { Write-Host "ok    $msg" }

Write-Host "stage: $Stage"
$plug = Join-Path $Stage "SKSE\Plugins\Magelight"
$hostDll = Join-Path $Stage "SKSE\Plugins\Magelight.dll"

# 1. runtime names
$expected = @("Magelight1.dll", "Magelight1Core.dll", "MgWCore.dll", "MagelightGPU.dll")
$stock = @("Ultralight.dll", "UltralightCore.dll", "WebCore.dll", "AppCore.dll")   # Ultralight's original names
$retired = @("MgACore.dll")   # the renamed AppCore.dll an older build shipped
foreach ($n in $expected) { if (Test-Path (Join-Path $plug $n)) { Pass "stage has $n" } else { Fail "stage missing $n" } }
foreach ($n in $stock) { if (Test-Path (Join-Path $plug $n)) { Fail "original Ultralight DLL name in stage: $n (breaks coexistence with other Ultralight-based UI mods)" } }
foreach ($n in $retired) { if (Test-Path (Join-Path $plug $n)) { Fail "retired runtime file in stage: $n (AppCore is compiled out; build.ps1 deletes it from a reused stage)" } }

# 2. original and retired names inside staged binaries. ASCII decoding keeps zero bytes, so a UTF-16 name is
#    found as its letters interleaved with NULs at any alignment.
function Find-Name([string]$hay, [string]$name) {
    $i = $hay.IndexOf($name, [System.StringComparison]::OrdinalIgnoreCase)
    if ($i -ge 0) { return @{ At = $i; Form = "ASCII" } }
    $i = $hay.IndexOf(($name.ToCharArray() -join "`0"), [System.StringComparison]::OrdinalIgnoreCase)
    if ($i -ge 0) { return @{ At = $i; Form = "UTF-16" } }
    return $null
}
$dlls = @($hostDll) + ($expected | ForEach-Object { Join-Path $plug $_ })
foreach ($d in $dlls) {
    if (-not (Test-Path $d)) { continue }
    $ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($d))
    $clean = $true
    foreach ($n in ($stock + $retired)) {
        # 'UltralightCore.dll' contains 'Ultralight' but not 'Ultralight.dll', so a whole-name hit is always a leftover.
        if ($hit = Find-Name $ascii $n) {
            Fail ("module name '{0}' ({1}) found inside {2} at 0x{3:X}" -f $n, $hit.Form, (Split-Path -Leaf $d), $hit.At)
            $clean = $false
        }
    }
    if ($clean) { Pass ("no original or retired module names in " + (Split-Path -Leaf $d)) }
}

# 3. required files
$required = @($hostDll, (Join-Path $plug "resources\icudt67l.dat"), (Join-Path $plug "resources\cacert.pem"),
              (Join-Path $Stage "Interface\magelightfocus.swf"),
              (Join-Path $plug "license\EULA.txt"), (Join-Path $plug "license\NOTICES.txt"),
              (Join-Path $plug "license\LICENSE.txt"),
              (Join-Path $plug "license\THIRD_PARTY_LICENSES.txt"), (Join-Path $plug "license\Ultralight-NOTICES.md"),
              (Join-Path $plug "license\Ultralight-LICENSE.txt"))
if (-not $NoPex) { $required += Join-Path $Stage "Scripts\Magelight.pex" }
foreach ($f in $required) {
    if (Test-Path $f) { Pass ("present: " + ($f.Substring($Stage.Length + 1))) } else { Fail "missing: $f" }
}

# 4. version coherence
$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
$m = [regex]::Match($cmake, 'VERSION\s+(\d+\.\d+\.\d+)')
if (-not $m.Success) { Fail "CMakeLists.txt: no project VERSION" } else {
    $ver = $m.Groups[1].Value
    if (Test-Path $hostDll) {
        $ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($hostDll))
        if ($ascii.Contains("Magelight v" + ($ver -replace '\.', '-')) -or $ascii.Contains($ver)) { Pass "host DLL carries version $ver" }
        else { Fail "host DLL does not contain version string $ver (stale build?)" }
    }
    $gpuDllFile = Join-Path $plug "MagelightGPU.dll"
    if (Test-Path $gpuDllFile) {
        $gpuVer = (Get-Item $gpuDllFile).VersionInfo.FileVersion
        if ($gpuVer -eq $ver -or $gpuVer -eq "$ver.0") { Pass "GPU driver FileVersion $gpuVer" }
        else { Fail "MagelightGPU.dll FileVersion '$gpuVer' != CMake VERSION $ver (rebuild the driver)" }
    }
}

# 4b. GPU driver exports (read from the PE export directory, not a string search)
function Get-PeExportNames([string]$path) {
    $b = [System.IO.File]::ReadAllBytes($path)
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    if ([BitConverter]::ToUInt32($b, $pe) -ne 0x4550) { return $null }
    $nSec = [BitConverter]::ToUInt16($b, $pe + 6)
    $optSize = [BitConverter]::ToUInt16($b, $pe + 20)
    $opt = $pe + 24
    $magic = [BitConverter]::ToUInt16($b, $opt)
    $dirs = if ($magic -eq 0x20B) { $opt + 112 } else { $opt + 96 }
    $expRva = [BitConverter]::ToUInt32($b, $dirs)
    if ($expRva -eq 0) { return @() }
    $secTab = $opt + $optSize
    $toOff = {
        param([uint32]$rva)
        for ($i = 0; $i -lt $nSec; $i++) {
            $s = $secTab + 40 * $i
            $va = [BitConverter]::ToUInt32($b, $s + 12)
            $size = [Math]::Max([BitConverter]::ToUInt32($b, $s + 8), [BitConverter]::ToUInt32($b, $s + 16))
            if ($rva -ge $va -and $rva -lt $va + $size) { return [BitConverter]::ToUInt32($b, $s + 20) + ($rva - $va) }
        }
        return -1
    }
    $exp = & $toOff $expRva
    if ($exp -lt 0) { return $null }
    $count = [BitConverter]::ToUInt32($b, $exp + 24)
    $namesOff = & $toOff ([BitConverter]::ToUInt32($b, $exp + 32))
    $names = @()
    for ($i = 0; $i -lt $count; $i++) {
        $o = & $toOff ([BitConverter]::ToUInt32($b, $namesOff + 4 * $i))
        $e = $o; while ($b[$e] -ne 0) { $e++ }
        $names += [System.Text.Encoding]::ASCII.GetString($b, $o, $e - $o)
    }
    return $names
}
$gpuDll = Join-Path $plug "MagelightGPU.dll"
if (Test-Path $gpuDll) {
    $exports = Get-PeExportNames $gpuDll
    if ($null -eq $exports) { Fail "MagelightGPU.dll: no readable PE export directory" } else {
        # Required ones (the host falls back to the CPU path without them), then the optional ones.
        $wanted = @("MgGpu_GetInfo", "MgGpu_Create", "MgGpu_Destroy", "MgGpu_GetGPUDriver", "MgGpu_HasCommandsPending",
                    "MgGpu_DrawCommandList", "MgGpu_GetTextureSRV", "MgGpu_SetSampleCount",
                    "MgGpu_RegisterExternalTexture", "MgGpu_SetExternalTextureSRV", "MgGpu_UnregisterExternalTexture",
                    "MgGpu_GetStats")
        $missing = @($wanted | Where-Object { $exports -notcontains $_ })
        if ($missing.Count -eq 0) { Pass ("GPU driver exports all {0} MgGpu_* functions the host resolves" -f $wanted.Count) }
        else { Fail ("MagelightGPU.dll lacks export(s): " + ($missing -join ", ") + " (stale driver build?)") }
    }
}

# 5. vendored header sync
if ($SaHeader -ne "") {
    $ours = Get-Content (Join-Path $root "api\MagelightUI_API.h") -Raw
    if (Test-Path $SaHeader) {
        $theirs = Get-Content $SaHeader -Raw
        if (($ours -replace "`r`n", "`n") -eq ($theirs -replace "`r`n", "`n")) { Pass "API header in sync with $SaHeader" }
        else { Fail "API header differs from the SA vendored copy: $SaHeader" }
    } else { Fail "SA header not found: $SaHeader" }
}

# 6. host DLL without the runtime. A copy alone in an empty folder, loaded by a child process: the runtime is on
#    none of its search paths. The "attach" child ends with TerminateProcess, so only the attach is judged. The
#    "exit" child ends with ExitProcess, which runs DLL_PROCESS_DETACH and the host's static destructors: one that
#    reaches a delay-loaded runtime import raises 0xC06D007E there, and can leave the child hung, hence the timeout.
#    The "create" child calls the v1 table's CreateView (MagelightApi1: apiVersion, hostVersion, then CreateView),
#    which must return a view id without reaching the runtime. The host's log lines go to the child's stdout too, so
#    each child is judged by the lines it must print, not by its whole output.
if (Test-Path $hostDll) {
    if (-not [Environment]::Is64BitProcess) { Fail "host DLL attach: run check_stage from a 64-bit PowerShell" } else {
        $tmp = Join-Path ([IO.Path]::GetTempPath()) ("mg-attach-" + [guid]::NewGuid().ToString("N"))
        New-Item -ItemType Directory -Path $tmp | Out-Null
        try {
            Copy-Item $hostDll (Join-Path $tmp "Magelight.dll")
            $probe = Join-Path $tmp "attach.ps1"
            Set-Content -Path $probe -Encoding ASCII -Value @'
param([string]$Mode)
$k = Add-Type -PassThru -Name Attach -Namespace MgStage -MemberDefinition @"
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
[DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
[DllImport("kernel32.dll")] public static extern bool TerminateProcess(IntPtr process, uint code);
[DllImport("kernel32.dll")] public static extern void ExitProcess(uint code);
"@
$m = $k::LoadLibraryExW((Join-Path $PSScriptRoot "Magelight.dll"), [IntPtr]::Zero, 0)
$err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
[Console]::Out.WriteLine($(if ($m -ne [IntPtr]::Zero) { "loaded" } else { "error $err" }))
[Console]::Out.Flush()
$code = $(if ($m -ne [IntPtr]::Zero) { 0 } else { 3 })
if ($Mode -eq "create" -and $m -ne [IntPtr]::Zero) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
namespace MgStage {
    public static class Create {
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true)] static extern IntPtr GetProcAddress(IntPtr module, string name);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr RequestApi(uint version);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate ulong CreateView([MarshalAs(UnmanagedType.LPStr)] string htmlPath,
            int x, int y, int w, int h, IntPtr onDomReady, [MarshalAs(UnmanagedType.I1)] bool clickThrough, [MarshalAs(UnmanagedType.I1)] bool startVisible);
        public static string Run(IntPtr module) {
            IntPtr req = GetProcAddress(module, "Magelight_RequestApi");
            if (req == IntPtr.Zero) return "no Magelight_RequestApi";
            IntPtr api = ((RequestApi)Marshal.GetDelegateForFunctionPointer(req, typeof(RequestApi)))(1);
            if (api == IntPtr.Zero) return "no v1 API";
            CreateView create = (CreateView)Marshal.GetDelegateForFunctionPointer(Marshal.ReadIntPtr(api, 16), typeof(CreateView));
            try {
                return create("views/check.html", 0, 0, 64, 64, IntPtr.Zero, false, false) != 0 ? "created" : "CreateView returned 0";
            } catch (SEHException e) {
                return "CreateView raised a native exception: " + e.Message;
            }
        }
    }
}
"@
    $said = [MgStage.Create]::Run($m)
    [Console]::Out.WriteLine($said)
    [Console]::Out.Flush()
    if ($said -ne "created") { $code = 5 }
}
if ($Mode -eq "exit") {
    $k::ExitProcess($code)
    # ExitProcess came back: something raised in it, which PowerShell turned into an error and went on.
    [void]$k::TerminateProcess($k::GetCurrentProcess(), 4)
}
[void]$k::TerminateProcess($k::GetCurrentProcess(), $code)
'@
            $exe = (Get-Process -Id $PID).Path
            $runs = @(
                @{ Mode = "attach"; Pass = "host DLL loads with the runtime out of reach";
                   Fail = "host DLL does not load without the runtime ({0}, exit {1}): a static constructor runs Ultralight code in DllMain?" },
                @{ Mode = "exit"; Pass = "a process that loaded the host DLL without the runtime exits cleanly";
                   Fail = "a process that loaded the host DLL without the runtime does not exit cleanly ({0}, exit {1}): a static destructor runs Ultralight code at DLL_PROCESS_DETACH?" },
                @{ Mode = "create"; Pass = "the host without the runtime registers a view (CreateView)"; Lines = @("loaded", "created");
                   Fail = "the host without the runtime fails CreateView ({0}, exit {1}): a view record that runs Ultralight code when it is built?" }
            )
            foreach ($run in $runs) {
                $out = Join-Path $tmp ("out-" + $run.Mode + ".txt")
                $p = Start-Process -FilePath $exe -ArgumentList @("-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", "`"$probe`"", $run.Mode) `
                    -WorkingDirectory $tmp -NoNewWindow -PassThru -RedirectStandardOutput $out `
                    -RedirectStandardError (Join-Path $tmp ("err-" + $run.Mode + ".txt"))
                $null = $p.Handle  # keeps ExitCode readable once the child is gone (Windows PowerShell)
                $exited = $p.WaitForExit(60000)
                if (-not $exited) { try { $p.Kill() } catch { } }
                $lines = @(Get-Content $out -ErrorAction SilentlyContinue | ForEach-Object { $_.Trim() } | Where-Object { $_ })
                $said = $lines -join " | "
                $exit = if ($exited) { "0x{0:X8}" -f $p.ExitCode } else { "none, killed after 60 s" }
                $want = if ($run.Lines) { $run.Lines } else { @("loaded") }
                $missing = @($want | Where-Object { $lines -notcontains $_ })
                if ($exited -and $p.ExitCode -eq 0 -and $missing.Count -eq 0) { Pass $run.Pass }
                else { Fail ($run.Fail -f $said, $exit) }
            }
        } finally {
            Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
        }
    }
}

if ($fail -gt 0) { Write-Host "`n$fail check(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host "`nAll stage checks passed" -ForegroundColor Green
exit 0
