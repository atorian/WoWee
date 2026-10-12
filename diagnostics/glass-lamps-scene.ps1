param([int]$AA=2, [int]$RT=3, [string]$View='window')
$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
$root = 'D:\games\WoWee'
$runtime = "$root\build\glass-lamps-runtime"
$env:WOW_DATA_PATH = "$root\Data"
$env:WOWEE_CONFIG_ROOT = "$root\build\glass-lamps-scene-$AA-$RT-config"
New-Item -ItemType Directory -Force -Path $env:WOWEE_CONFIG_ROOT | Out-Null
Set-Content "$env:WOWEE_CONFIG_ROOT\settings.cfg" "antialiasing=$AA`nray_traced_lighting=$RT`nfov=110" -Encoding ASCII
$env:WOWEE_SKY_BACKGROUND_PROBE='1'
$env:WOWEE_FOUNTAIN_PROBE='1'
$env:WOWEE_GLASS_LAMP_PROBE=$View
$env:WOWEE_SINGLE_THREAD_RECORD='1'
$env:WOWEE_VULKAN_VALIDATION='1'
$env:VK_LAYER_PATH="$root\build\validation-runtime\mingw64\bin"
$env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'
$env:VK_LOADER_LAYERS_DISABLE='~implicit~'
$env:WOWEE_LOG_FILE="glass-lamps-scene-$View-$AA-$RT.log"
$env:WOWEE_LOG_LEVEL='info'
$env:WOWEE_LOG_STDOUT='0'
$env:WOWEE_RECORD="$root\build\glass-lamps-scene-$View-$AA-$RT.mp4"
$env:WOWEE_RECORD_SECONDS='1.0'
Remove-Item Env:WOWEE_SCREENSHOT -ErrorAction SilentlyContinue
Copy-Item "$root\build\bin\wowee-glass-lamps-validated.exe" $runtime -Force
Copy-Item "$root\build\bin\assets\*" "$runtime\assets" -Recurse -Force
Copy-Item "$root\build\glass-lamps-shaders\*" "$runtime\assets\shaders" -Force
$p=Start-Process "$runtime\wowee-glass-lamps-validated.exe" -WorkingDirectory $runtime -PassThru -RedirectStandardOutput "$root\build\glass-lamps-scene-$View-$AA-$RT.out" -RedirectStandardError "$root\build\glass-lamps-scene-$View-$AA-$RT.err"
$null=$p.Handle
if (-not $p.WaitForExit(60000)) { throw "Scene still running: PID=$($p.Id)" }
$p.Refresh()
if ($p.ExitCode -ne 0) { throw "Scene exit=$($p.ExitCode)" }
$log=Get-Content "$runtime\logs\$env:WOWEE_LOG_FILE" -Raw
if ($log -notmatch 'Glass/lamp GPU probe: real Dalaran city groups=91 lamp batches=7') { throw 'Actual Dalaran fixture not loaded' }
if ($log -notmatch 'Application exited successfully') { throw 'No clean shutdown' }
$errors=$log -split "`n" | Where-Object { $_ -match '\[ERROR|Validation Error|VUID-' -and $_ -notmatch 'loader_get_json: Failed to open JSON file .*EOSOverlayVkLayer-Win(32|64)\.json' }
if ($errors) { $errors | Write-Output; throw 'Scene validation failed' }
Write-Output "Real Dalaran scene passed: AA=$AA RT=$RT view=$View"
