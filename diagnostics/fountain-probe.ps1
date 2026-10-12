param([string]$Label = 'before', [int]$AA = 2)
$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
$root = 'D:\games\WoWee'
$env:WOW_DATA_PATH = "$root\Data"
$env:WOWEE_CONFIG_ROOT = "$root\build\fountain-$Label-$AA-config"
New-Item -ItemType Directory -Force -Path $env:WOWEE_CONFIG_ROOT | Out-Null
Set-Content "$env:WOWEE_CONFIG_ROOT\settings.cfg" "antialiasing=$AA" -Encoding ASCII
$env:WOWEE_SKY_BACKGROUND_PROBE = '1'
$env:WOWEE_FOUNTAIN_PROBE = '1'
if ($Label -eq 'legacy') { $env:WOWEE_FOUNTAIN_PROBE_LEGACY = '1' } else { Remove-Item Env:WOWEE_FOUNTAIN_PROBE_LEGACY -ErrorAction SilentlyContinue }
$env:WOWEE_M2_BATCH_DIAG = 'dalaran_fountain'
$env:WOWEE_SINGLE_THREAD_RECORD = '1'
$env:WOWEE_VULKAN_VALIDATION = '1'
$env:VK_LAYER_PATH = "$root\build\validation-runtime\mingw64\bin"
$env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
$env:WOWEE_LOG_FILE = "fountain-$Label-$AA.log"
$env:WOWEE_LOG_LEVEL = 'info'
$env:WOWEE_LOG_STDOUT = '0'
$env:WOWEE_RECORD = "$root\build\fountain-$Label-$AA.mp4"
$env:WOWEE_RECORD_SECONDS = '0.2'
Remove-Item Env:WOWEE_SCREENSHOT -ErrorAction SilentlyContinue
$log = "$root\build\bin\logs\$env:WOWEE_LOG_FILE"
Remove-Item $env:WOWEE_RECORD, $log -Force -ErrorAction SilentlyContinue
$p = Start-Process "$root\build\bin\wowee-fountain-fix.exe" -WorkingDirectory "$root\build\bin" -PassThru -RedirectStandardOutput "$root\build\fountain-$Label-$AA.out" -RedirectStandardError "$root\build\fountain-$Label-$AA.err"
if (-not $p.WaitForExit(60000)) { throw "Fountain probe still running: PID=$($p.Id)" }
$text = Get-Content $log -Raw
if ($text -notmatch 'Fountain GPU probe: real Dalaran_Fountain_01 uploaded, batches=25') { throw 'Real fountain not loaded' }
if ($text -notmatch 'Application exited successfully') { throw 'No clean shutdown' }
$errors = $text -split "`n" | Where-Object { $_ -match '\[ERROR|Validation Error|VUID-' -and $_ -notmatch 'loader_get_json: Failed to open JSON file .*EOSOverlayVkLayer-Win(32|64)\.json' }
if ($errors) { $errors | Write-Output; throw 'GPU validation failed' }
if (-not (Test-Path $env:WOWEE_RECORD)) { throw 'No recording produced' }
Write-Output "Fountain probe passed: $Label, AA=$AA"
