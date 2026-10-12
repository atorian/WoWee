param([ValidateSet('baseline','fixed')][string]$Variant='fixed')
$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
$root = 'D:\games\WoWee'
$env:WOW_DATA_PATH = "$root\Data"
$env:WOWEE_MULGORE_PROBE = '1'
$env:WOWEE_ORGRIMMAR_PROBE = '1'
if ($Variant -eq 'baseline') { $env:WOWEE_ORGRIMMAR_PROBE_BASELINE='1' }
else { Remove-Item Env:WOWEE_ORGRIMMAR_PROBE_BASELINE -ErrorAction SilentlyContinue }
$env:WOWEE_SINGLE_THREAD_RECORD = '1'
$env:WOWEE_CONFIG_ROOT = "$root\build\orgrimmar-$Variant-config"
$env:APPDATA = $env:WOWEE_CONFIG_ROOT
$env:WOWEE_RECORD = "$root\build\orgrimmar-$Variant.mp4"
$env:WOWEE_RECORD_SECONDS = '10'
$env:WOWEE_LOG_FILE = "orgrimmar-$Variant.log"
$env:WOWEE_LOG_LEVEL = 'info'
$env:WOWEE_LOG_STDOUT = '0'
$env:WOWEE_VULKAN_VALIDATION = '1'
$env:VK_LAYER_PATH = "$root\build\validation-runtime\mingw64\bin"
$env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
Remove-Item Env:WOWEE_ENTITY_STREAM_PROBE,Env:WOWEE_SCREENSHOT -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $env:WOWEE_CONFIG_ROOT | Out-Null
Set-Content "$env:WOWEE_CONFIG_ROOT\settings.cfg" "antialiasing=3`nray_traced_lighting=0`nview_distance=2400`nfog_strength=0.5`nmist_density=2`nlight_shafts=2`nshadows=1`nshadow_distance=500`nnormal_mapping=1`nnormal_map_strength=1.2`nfov=70`nupscaling_mode=0`nfsr=0" -Encoding ASCII
$logPath = "$root\build\bin\logs\$env:WOWEE_LOG_FILE"
Remove-Item $logPath,$env:WOWEE_RECORD -Force -ErrorAction SilentlyContinue
$p = Start-Process "$root\build\bin\wowee-orgrimmar-light.exe" -WorkingDirectory "$root\build\bin" -PassThru -RedirectStandardOutput "$root\build\orgrimmar-$Variant.out" -RedirectStandardError "$root\build\orgrimmar-$Variant.err"
$null=$p.Handle
Write-Output "Orgrimmar $Variant PID=$($p.Id)"
if (-not $p.WaitForExit(180000)) { throw "Fixture still running PID=$($p.Id)" }
$p.Refresh()
if ($p.ExitCode -ne 0) { throw "Fixture exit=$($p.ExitCode)" }
$log = Get-Content $logPath -Raw
if ($log -notmatch 'Mulgore probe: world entry completed' -or $log -notmatch 'sky: zone=1637 hour=8.73333' -or $log -notmatch 'Light volumes on map 1:' -or $log -notmatch 'volumes=239/204' -or $log -notmatch 'Application exited successfully') { throw 'Real Orgrimmar/hour or clean shutdown missing' }
if (-not (Test-Path $env:WOWEE_RECORD)) { throw 'Recording missing' }
$errors = $log -split "`n" | Where-Object { $_ -match '\[ERROR|Validation Error|VUID-' -and $_ -notmatch 'loader_get_json: Failed to open JSON file .*EOSOverlayVkLayer-Win(32|64)\.json' }
if ($errors) { $errors | Select-Object -First 10 | Write-Output; throw 'Vulkan fixture failed' }
Write-Output "Passed real Orgrimmar 08:44 ${Variant}: world loading, frames, validation, clean shutdown"
