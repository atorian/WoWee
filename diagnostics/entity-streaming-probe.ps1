param([switch]$Unbatched, [switch]$Validate, [switch]$Expanded, [ValidateSet('wowee-entity-streaming.exe','wowee-streaming-batched-baseline.exe')][string]$Executable = 'wowee-entity-streaming.exe')
$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
$root = 'D:\games\WoWee'
$variant = if ($Unbatched) { 'unbatched' } else { 'batched' }
if ($Validate) { $variant += '-validated' }
if ($Expanded) { $variant += '-expanded' }
if ($Executable -eq 'wowee-streaming-batched-baseline.exe') { $variant += '-baseline' }
$env:WOW_DATA_PATH = "$root\Data"
$env:WOWEE_SKY_BACKGROUND_PROBE = '1'
$env:WOWEE_ENTITY_STREAM_PROBE = '1'
Remove-Item Env:WOWEE_ENTITY_STREAM_EXPANDED -ErrorAction SilentlyContinue
if ($Expanded) { $env:WOWEE_ENTITY_STREAM_EXPANDED = '1' }
$env:WOWEE_SINGLE_THREAD_RECORD = '1'
$env:WOWEE_RECORD_SECONDS = '8'
$env:WOWEE_CONFIG_ROOT = "$root\build\entity-streaming-$variant-config"
$env:WOWEE_RECORD = "$root\build\entity-streaming-$variant.mp4"
$env:WOWEE_LOG_FILE = "entity-streaming-$variant.log"
$env:WOWEE_LOG_LEVEL = if ($Expanded) { 'debug' } else { 'info' }
$env:WOWEE_LOG_STDOUT = '0'
Remove-Item Env:WOWEE_SCREENSHOT -ErrorAction SilentlyContinue
Remove-Item Env:WOWEE_VK_NO_UPLOAD_BATCH -ErrorAction SilentlyContinue
if ($Unbatched) { $env:WOWEE_VK_NO_UPLOAD_BATCH = '1' }
Remove-Item Env:WOWEE_VULKAN_VALIDATION,Env:VK_INSTANCE_LAYERS -ErrorAction SilentlyContinue
if ($Validate) {
  $env:WOWEE_VULKAN_VALIDATION = '1'
  $env:VK_LAYER_PATH = "$root\build\validation-runtime\mingw64\bin"
  $env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
  $env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
  if (-not (Test-Path "$env:VK_LAYER_PATH\VkLayer_khronos_validation.dll")) { throw 'Validation runtime missing' }
}
New-Item -ItemType Directory -Force -Path $env:WOWEE_CONFIG_ROOT | Out-Null
Set-Content -Path "$env:WOWEE_CONFIG_ROOT\settings.cfg" -Value "antialiasing=0`nray_traced_lighting=0`nupscaling_mode=0`nfsr=0" -Encoding ASCII
$log = "$root\build\bin\logs\$env:WOWEE_LOG_FILE"
Remove-Item $env:WOWEE_RECORD,$log -Force -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$root\build\bin\$Executable" -WorkingDirectory "$root\build\bin" -PassThru -RedirectStandardOutput "$root\build\entity-streaming-$variant.out" -RedirectStandardError "$root\build\entity-streaming-$variant.err"
if (-not $p.WaitForExit(60000)) { throw "Stream probe still running: PID=$($p.Id)" }
$text = Get-Content $log -Raw
if ($text -notmatch 'Entity stream probe: completed players=24 ') { throw "Queue did not finish: $log" }
if ($Expanded -and $text -notmatch 'Expanded entity probe: mounts=2 cancelled=1 replaced=1') { throw 'Mount/cancellation/replacement check failed' }
if ($Expanded -and [regex]::Matches($text, 'Attached weapon model ').Count -lt 24) { throw 'Expanded probe did not attach all 24 weapons' }
if ($text -notmatch 'Application exited successfully') { throw 'No clean probe shutdown' }
if (-not (Test-Path $env:WOWEE_RECORD)) { throw 'No GPU frame recording' }
$errors = $text -split "`n" | Where-Object { $_ -match '\[ERROR|Validation Error|VUID-' -and $_ -notmatch 'loader_get_json: Failed to open JSON file .*EOSOverlayVkLayer-Win(32|64)\.json' }
if ($errors) { $errors | Select-Object -First 8 | Write-Output; throw "GPU probe failed: $log" }
$text -split "`n" | Where-Object { $_ -match 'Entity stream probe: completed' } | Write-Output
Write-Output "Passed: $variant, 24 real players with chest and weapon, queue drained, clean shutdown"
exit 0
