param(
    [ValidateSet('before','after','thick')][string]$Variant = 'after',
    [ValidateSet('hyjal_day','citadel_evening','dalaran_gundrak')][string]$Scene = 'hyjal_day'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$output = "$root\build-dev\scratch\atmosphere"
New-Item -ItemType Directory -Force $output, "$root\build-dev\tmp" | Out-Null
$env:PATH = 'C:\msys64\mingw64\bin;' + $env:PATH
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:TMP = $env:TEMP = $env:TMPDIR = "$root\build-dev\tmp"
if (-not $env:WOW_DATA_PATH) { $env:WOW_DATA_PATH = 'D:\games\WoWee\Data' }
$env:WOWEE_RESOURCE_ROOT = "$root\build-dev\bin"
$env:WOWEE_CONFIG_ROOT = "$output\profile"
$env:WOWEE_LOG_LEVEL = 'info'
$env:WOWEE_VULKAN_VALIDATION = '0'
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
$env:WOWEE_RENDER_THREAD = '0'
Remove-Item Env:VK_INSTANCE_LAYERS -ErrorAction SilentlyContinue
Set-Location $root
& ninja -C build-dev scene_render -j 6
if ($LASTEXITCODE) { exit $LASTEXITCODE }
# Physical atmosphere now uses one compiled shader set. Historical prototype
# comparisons remain in scratch/aerial-march; this suite writes separately.
$enabled = if ($Variant -eq 'before') { 0 } else { 1 }
$scenePath = "$root\tools\scene_render\scenes\aerial_march\$Scene.json"
$sceneData = Get-Content -Raw $scenePath | ConvertFrom-Json
$sceneData | Add-Member -NotePropertyName aerialRayMarching -NotePropertyValue ([bool]$enabled) -Force
if ($Variant -eq 'thick') { $sceneData.fogStrength = 1.25 }
$scenePath = "$output\$Scene-$Variant.json"
$json = $sceneData | ConvertTo-Json -Depth 20
[System.IO.File]::WriteAllText($scenePath, $json, (New-Object System.Text.UTF8Encoding($false)))
Set-Location "$root\build-dev\bin"
& .\scene_render.exe $scenePath `
    "$output\$Scene-$Variant.png" --timing > "$output\$Scene-$Variant.log" 2>&1
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Write-Host "$output\$Scene-$Variant.png"
