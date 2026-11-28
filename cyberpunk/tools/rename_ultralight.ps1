# Gives each binary Magelight's names for Ultralight's three DLLs, as the Skyrim build does (../build.ps1): the loader
# binds an import by base name, so another mod's stock Ultralight.dll in the game would otherwise be ours. Every
# occurrence, ASCII and UTF-16, import and delay-load descriptors included; same length, so the PE layout is unchanged.
# Longest name first, so "UltralightCore.dll" is never half-eaten by the "Ultralight.dll" pass. Idempotent.
param([Parameter(Mandatory, ValueFromRemainingArguments)] [string[]] $Files)
$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
public static class MlcpPatch {
    public static int ReplaceAll(byte[] hay, byte[] from, byte[] to) {
        int count = 0;
        for (int i = 0; i + from.Length <= hay.Length; i++) {
            bool hit = true;
            for (int j = 0; j < from.Length; j++) { if (hay[i + j] != from[j]) { hit = false; break; } }
            if (hit) { System.Array.Copy(to, 0, hay, i, to.Length); count++; i += from.Length - 1; }
        }
        return count;
    }
}
"@

$renames = [ordered]@{
    "UltralightCore.dll" = "Magelight1Core.dll"
    "Ultralight.dll"     = "Magelight1.dll"
    "WebCore.dll"        = "MgWCore.dll"
}
foreach ($path in $Files) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $total = 0
    foreach ($k in $renames.Keys) {
        foreach ($enc in [System.Text.Encoding]::ASCII, [System.Text.Encoding]::Unicode) {
            $total += [MlcpPatch]::ReplaceAll($bytes, $enc.GetBytes($k), $enc.GetBytes($renames[$k]))
        }
    }
    if ($total) { [System.IO.File]::WriteAllBytes($path, $bytes) }
    Write-Host ("rename_ultralight: " + (Split-Path $path -Leaf) + " " + $total + " name refs")
}
