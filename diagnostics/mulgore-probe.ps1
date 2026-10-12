param([switch]$Validate)
$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
$root = 'D:\games\WoWee'
$variant = if ($Validate) { 'validated' } else { 'fixed' }
$env:WOW_DATA_PATH = "$root\Data"
$env:WOWEE_MULGORE_PROBE = '1'
$env:WOWEE_ENTITY_STREAM_PROBE = '1'
$env:WOWEE_SINGLE_THREAD_RECORD = '1'
$env:WOWEE_CONFIG_ROOT = "$root\build\mulgore-$variant-config"
$env:APPDATA = $env:WOWEE_CONFIG_ROOT
$env:WOWEE_RECORD = "$root\build\mulgore-$variant.mp4"
$env:WOWEE_RECORD_SECONDS = '8'
$env:WOWEE_LOG_FILE = "mulgore-$variant.log"
$env:WOWEE_LOG_LEVEL = 'debug'
$env:WOWEE_LOG_STDOUT = '0'
if ($Validate) {
    $env:WOWEE_VULKAN_VALIDATION = '1'
    $env:VK_LAYER_PATH = "$root\build\validation-runtime\mingw64\bin"
    $env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
    $env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
    if (-not (Test-Path "$env:VK_LAYER_PATH\VkLayer_khronos_validation.dll")) { throw 'Validation runtime missing' }
}
New-Item -ItemType Directory -Force -Path $env:WOWEE_CONFIG_ROOT | Out-Null
Set-Content "$env:WOWEE_CONFIG_ROOT\settings.cfg" "antialiasing=3`nray_traced_lighting=0`nview_distance=2400`nupscaling_mode=0`nfsr=0" -Encoding ASCII
$logPath = "$root\build\bin\logs\$env:WOWEE_LOG_FILE"
Remove-Item $logPath,$env:WOWEE_RECORD,"$env:APPDATA\wowee\last_world.cfg" -Force -ErrorAction SilentlyContinue
$p = Start-Process "$root\build\bin\wowee-mulgore-fix.exe" -WorkingDirectory "$root\build\bin" -PassThru -RedirectStandardOutput "$root\build\mulgore-$variant.out" -RedirectStandardError "$root\build\mulgore-$variant.err"
Write-Output "Mulgore probe PID=$($p.Id)"
if (-not $p.WaitForExit(120000)) { throw "Probe stalled, PID=$($p.Id)" }
if ($null -ne $p.ExitCode -and $p.ExitCode -ne 0) { throw "Probe exit=$($p.ExitCode)" }
if (-not (Test-Path $env:WOWEE_RECORD) -or (Get-Item $env:WOWEE_RECORD).Length -eq 0) { throw 'No GPU recording produced' }
$log = Get-Content $logPath -Raw
if ($log -notmatch 'Mulgore probe: world entry completed' -or $log -notmatch 'Application exited successfully' -or $log -notmatch 'Entity stream probe: completed players=24 ') { throw 'World entry did not complete cleanly' }
$errors = $log -split "`n" | Where-Object { $_ -match '\[ERROR|Validation Error|VUID-' -and $_ -notmatch 'loader_get_json: Failed to open JSON file .*EOSOverlayVkLayer-Win(32|64)\.json' }
if ($errors) { $errors | Select-Object -First 10 | Write-Output; throw 'GPU probe errors' }
Write-Output 'Passed: Mulgore entry, GPU frames and clean shutdown'
