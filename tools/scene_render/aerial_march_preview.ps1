param(
    [ValidateSet('before','after','reference')][string]$Variant = 'after',
    [ValidateSet('hyjal_day','citadel_evening')][string]$Scene = 'hyjal_day'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$output = "$root\build-dev\scratch\aerial-march"
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
# Keep variants out of both the source SPIR-V and the normal client's assets.
$enabled = if ($Variant -eq 'before') { 0 } else { 1 }
$steps = if ($Variant -eq 'reference') { 48 } else { 24 }
foreach ($shader in @('terrain','distant_terrain','wmo','m2','character','water','grass')) {
    & glslc -O -fshader-stage=frag "-DAERIAL_MARCH_ENABLED=$enabled" "-DAERIAL_MARCH_STEPS=$steps" `
      "-I$root\assets\shaders" "$root\assets\shaders\$shader.frag.glsl" `
      -o "$root\build-dev\bin\assets\shaders\$shader.frag.spv"
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & spirv-val "$root\build-dev\bin\assets\shaders\$shader.frag.spv"
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
Set-Location "$root\build-dev\bin"
& .\scene_render.exe "$root\tools\scene_render\scenes\aerial_march\$Scene.json" `
    "$output\$Scene-$Variant.png" --timing > "$output\$Scene-$Variant.log" 2>&1
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Write-Host "$output\$Scene-$Variant.png"
