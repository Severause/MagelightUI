# Runs the bench on two Ultralight SDK builds and writes every RESULT line to <Out>\results.txt.
#   run.ps1 -SdkA <dir> -GpuA <MagelightGPU.dll built for A> -SdkB <dir> -GpuB <dll built for B> [-View <built view folder>]
#           [-Out C:\b\mgl-bench] [-Passes 3] [-Frames 420] [-Only name,name]
# Each SDK is paired with the driver built against it (two betas of one version cannot be told apart from the DLL).
# The passes alternate between the two, so a slow drift of the PC lands on both. compare.py reads the results.
param(
    [Parameter(Mandatory)][string]$SdkA,
    [Parameter(Mandatory)][string]$GpuA,
    [Parameter(Mandatory)][string]$SdkB,
    [Parameter(Mandatory)][string]$GpuB,
    [string]$View = "",
    [string]$Out = "C:\b\mgl-bench",
    [int]$Passes = 3,
    [int]$Frames = 420,
    [string[]]$Only = @()
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$arms = @(@{ sdk = $SdkA; gpu = $GpuA }, @{ sdk = $SdkB; gpu = $GpuB })
foreach ($a in $arms) {
    $a.label = (Get-Content (Join-Path $a.sdk 'VERSION.txt') -TotalCount 1).Trim()
    $a.dir = Join-Path $Out $a.label
    & cmd /c "`"$here\build.bat`" `"$($a.sdk)`" `"$($a.dir)`" `"$View`" > `"$Out-build-$($a.label).log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { throw "build.bat failed for $($a.label): see $Out-build-$($a.label).log" }
    New-Item -ItemType Directory -Force (Join-Path $a.dir 'gpu') | Out-Null
    Copy-Item $a.gpu (Join-Path $a.dir 'gpu\MagelightGPU.dll') -Force
}
# name, url, width, height, device scale, renderer threads (0 = the SDK's default), extra arguments
$scenarios = @(
    @('hud-1.0',    'file:///pages/hud.html',        2560, 1440, 1.0,  0, @()),
    @('hud-1.25',   'file:///pages/hud.html',        2560, 1440, 1.25, 0, @()),
    @('hud-1.5',    'file:///pages/hud.html',        2560, 1440, 1.5,  0, @()),
    @('list',       'file:///pages/list.html',       1920, 1080, 1.0,  0, @()),
    @('list-1.5',   'file:///pages/list.html',       2560, 1440, 1.5,  0, @()),
    @('images',     'file:///pages/images.html',     1920, 1080, 1.0,  0, @()),
    @('shadows',    'file:///pages/shadows.html',    1920, 1080, 1.0,  0, @()),
    @('matchmedia', 'file:///pages/matchmedia.html', 1920, 1080, 1.0,  0, @()),
    @('anim',       'file:///pages/anim.html',       1920, 1080, 1.0,  0, @()),
    @('list@t1',    'file:///pages/list.html',       1920, 1080, 1.0,  1, @()),
    @('list@t2',    'file:///pages/list.html',       1920, 1080, 1.0,  2, @()),
    @('list@t4',    'file:///pages/list.html',       1920, 1080, 1.0,  4, @())
)
if ($View) {
    $tick = @('--tickfile', (Join-Path $here 'sa-tick.js'), '--mouse')
    $scenarios += , @('view',    'file:///sa/index.html', 2560, 1440, 1.0, 0, $tick)
    $scenarios += , @('view@t1', 'file:///sa/index.html', 2560, 1440, 1.0, 1, $tick)
    $scenarios += , @('view@t2', 'file:///sa/index.html', 2560, 1440, 1.0, 2, $tick)
}
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # -File passes "a,b" as one string
if ($Only.Count) { $scenarios = @($scenarios | Where-Object { $Only -contains $_[0] }) }
$results = Join-Path $Out 'results.txt'
Set-Content $results "" -Encoding ascii
for ($p = 1; $p -le $Passes; $p++) {
    foreach ($s in $scenarios) {
        foreach ($a in $arms) {
            Set-Location $a.dir   # Ultralight writes its log into the working folder
            $args2 = @((Join-Path $a.dir 'assets'), $s[1], '--gpu', (Join-Path $a.dir 'gpu'), '--name', $s[0], '--sdk', $a.label,
                       '--w', $s[2], '--h', $s[3], '--scale', $s[4], '--threads', $s[5], '--frames', $Frames) + $s[6]
            $lines = & (Join-Path $a.dir 'bench.exe') @args2   # stdout only: a native stderr line would end the script
            $r = $lines | Where-Object { $_ -like 'RESULT *' }
            if ($r) { Add-Content $results "$r pass=$p" -Encoding ascii }
            else { Add-Content $results "FAILED name=$($s[0]) sdk=$($a.label) pass=$p exit=$LASTEXITCODE $($lines | Select-Object -Last 1)" -Encoding ascii }
        }
    }
    Write-Host "pass $p of $Passes done"
}
Write-Host "results: $results"
