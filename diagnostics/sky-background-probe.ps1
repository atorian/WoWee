$ErrorActionPreference = 'Stop'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
$root = 'D:\games\WoWee'
$env:WOW_DATA_PATH = "$root\Data"
$env:WOWEE_SKY_BACKGROUND_PROBE = '1'
$env:WOWEE_SINGLE_THREAD_RECORD = '1'
$env:WOWEE_RECORD_SECONDS = '0.5'
Remove-Item Env:WOWEE_SCREENSHOT -ErrorAction SilentlyContinue
$env:WOWEE_VULKAN_VALIDATION = '1'
$env:VK_LAYER_PATH = "$root\build\validation-runtime\mingw64\bin"
$env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
if (-not (Test-Path "$env:VK_LAYER_PATH\VkLayer_khronos_validation.dll")) { throw 'Local validation runtime missing' }
$env:WOWEE_LOG_LEVEL = 'info'
$env:WOWEE_LOG_STDOUT = '0'
foreach ($aa in @(0, 2, 3)) {
  $env:WOWEE_CONFIG_ROOT = "$root\build\sky-background-probe-$aa-config"
  New-Item -ItemType Directory -Force -Path $env:WOWEE_CONFIG_ROOT | Out-Null
  Set-Content -Path "$env:WOWEE_CONFIG_ROOT\settings.cfg" -Value "antialiasing=$aa" -Encoding ASCII
  $env:WOWEE_LOG_FILE = "sky-background-probe-$aa.log"
  $env:WOWEE_RECORD = "$root\build\sky-background-probe-$aa.mp4"
  $log = "$root\build\bin\logs\$env:WOWEE_LOG_FILE"
  Remove-Item $env:WOWEE_RECORD, $log -Force -ErrorAction SilentlyContinue
  $p = Start-Process -FilePath "$root\build\bin\wowee-distance-fog.exe" -WorkingDirectory "$root\build\bin" -PassThru -RedirectStandardOutput "$root\build\sky-background-probe-$aa.out" -RedirectStandardError "$root\build\sky-background-probe-$aa.err"
  if (-not $p.WaitForExit(60000)) { throw "GPU probe still running: PID=$($p.Id)" }
  $text = Get-Content $log -Raw
  if (-not (Test-Path $env:WOWEE_RECORD)) { throw 'No probe frame recording' }
  if ($text -notmatch 'Sky background GPU probe: captured current sky, authored=1') { throw 'Authored Dalaran sky was not captured' }
  if ($text -notmatch 'Application exited successfully') { throw 'No clean probe shutdown' }
  # Broken system Epic overlay manifests are loader diagnostics, not validation
  # errors from the renderer; never ignore any VUID or other application error.
  $errors = $text -split "`n" | Where-Object { $_ -match '\[ERROR|Validation Error|VUID-' -and $_ -notmatch 'loader_get_json: Failed to open JSON file .*EOSOverlayVkLayer-Win(32|64)\.json' }
  if ($errors) { throw "GPU validation failed: $log" }
  Write-Output "Authored sky capture passed (inline command recording): antialiasing=$aa, frame slots, clean shutdown, Vulkan validation"
}
