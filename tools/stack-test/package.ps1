# package.ps1 - zips the MgStackTest manifest mod for a mod manager: MgStackTest-<version>.zip, the version from its
# manifest, holding Magelight/MgStackTest/... with '/' separators (System.IO.Compression; Compress-Archive writes '\').
#   .\tools\stack-test\package.ps1 [-Out <folder>]   (default: .\dist)
param([string]$Out = "")
$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Out) { $Out = Join-Path $root "dist" }
$mod = Join-Path $PSScriptRoot "MgStackTest"
$version = (Get-Content (Join-Path $mod "manifest.json") -Raw | ConvertFrom-Json).version
if (-not $version) { throw "MgStackTest/manifest.json has no version" }
New-Item -ItemType Directory -Path $Out -Force | Out-Null
$zip = Join-Path $Out "MgStackTest-$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $base = (Resolve-Path $mod).Path.TrimEnd('\') + '\'
    foreach ($f in Get-ChildItem $mod -Recurse -File) {
        $entry = "Magelight/MgStackTest/" + $f.FullName.Substring($base.Length).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry) | Out-Null
    }
} finally {
    $archive.Dispose()
}
Write-Host "wrote $zip"
