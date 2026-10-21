<#
.SYNOPSIS
Builds scene_render and renders the isolated cloud suite. Never starts the game.
.EXAMPLE
    .\tools\scene_render\clouds.ps1
.EXAMPLE
    .\tools\scene_render\clouds.ps1 -BuildDir 'D:\games\WoWee\.claude\worktrees\hungry-wilson-e5280c\build-dev'
#>
param([string]$BuildDir, [string]$Output, [string]$Baseline)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $BuildDir) { $BuildDir = Join-Path $root 'build-dev' }
$runs = Join-Path $root 'build-dev\scratch\cloud-tests'
$tmp = Join-Path $root 'build-dev\tmp'
New-Item -ItemType Directory -Force $runs,$tmp | Out-Null
if (-not $Output) { $Output = Join-Path $runs (Get-Date -Format 'yyyyMMdd-HHmmss-fff') }
$latest = Join-Path $runs 'latest.json'
if (-not $Baseline -and (Test-Path $latest)) {
    $previous = Get-Content $latest -Raw | ConvertFrom-Json
    if (Test-Path (Join-Path $previous.output 'results.json')) { $Baseline = $previous.output }
}
$env:PATH = 'C:\msys64\mingw64\bin;' + $env:PATH
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:TMP = $tmp; $env:TEMP = $tmp; $env:TMPDIR = $tmp
$env:WOW_DATA_PATH = Join-Path $root 'Data'
$env:WOWEE_VULKAN_VALIDATION = '0'
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
Remove-Item Env:VK_INSTANCE_LAYERS -ErrorAction SilentlyContinue
& ninja -C $BuildDir -j4 scene_render test_sky_celestial
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& ctest --test-dir $BuildDir -R '^sky_celestial$' --output-on-failure
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$arguments = @((Join-Path $PSScriptRoot 'check_clouds.py'),
               (Join-Path $BuildDir 'bin\scene_render.exe'), $Output)
if ($Baseline) { $arguments += @('--baseline', $Baseline) }
& python @arguments
$result = $LASTEXITCODE
if (Test-Path (Join-Path $Output 'results.json')) {
    @{output=[System.IO.Path]::GetFullPath($Output); exitCode=$result} |
        ConvertTo-Json | Set-Content -Encoding UTF8 $latest
}
Write-Host "Cloud gallery: $(Join-Path $Output 'index.html')"
exit $result
