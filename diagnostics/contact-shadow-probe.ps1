param([ValidateSet('brill','stormwind','dalaran','sholazar')][string]$Scene='brill', [ValidateSet('before','after')][string]$Variant='after', [switch]$Legacy, [ValidateRange(0,23.99)][double]$Hour=12.0, [ValidateRange(-1024,1024)][double]$OffsetX=0, [ValidateRange(-1024,1024)][double]$OffsetY=0)
$ErrorActionPreference='Stop'
$env:PATHEXT='.COM;.EXE;.BAT;.CMD'
$env:Path='C:\msys64\mingw64\bin;C:\msys64\usr\bin;'+$env:Path
$root='D:\games\WoWee'
$label="solar-$Scene-$Variant-$($Hour.ToString([System.Globalization.CultureInfo]::InvariantCulture))" + $(if($Legacy){'-legacy'}else{''})
if($OffsetX -ne 0 -or $OffsetY -ne 0){$label += "-offset-$OffsetX-$OffsetY"}
$env:WOWEE_SHADOW_PROBE_OFFSET_X=$OffsetX.ToString([System.Globalization.CultureInfo]::InvariantCulture)
$env:WOWEE_SHADOW_PROBE_OFFSET_Y=$OffsetY.ToString([System.Globalization.CultureInfo]::InvariantCulture)
$runtime="$root\build\$label-runtime"
New-Item -ItemType Directory -Force -Path $runtime | Out-Null
Copy-Item "$root\build\pr38\bin\assets" $runtime -Recurse -Force
Copy-Item "$root\build\pr38\bin\wowee.exe" "$runtime\wowee.exe" -Force
$env:WOW_DATA_PATH="$root\Data"
$env:WOWEE_MULGORE_PROBE='1'
$env:WOWEE_SHADOW_PROBE_SCENE=$Scene
$env:WOWEE_SHADOW_PROBE_HOUR=$Hour.ToString([System.Globalization.CultureInfo]::InvariantCulture)
Remove-Item Env:WOWEE_ORGRIMMAR_PROBE,Env:WOWEE_ORGRIMMAR_SHADOW_PROBE,Env:WOWEE_ENTITY_STREAM_PROBE,Env:WOWEE_SCREENSHOT -ErrorAction SilentlyContinue
if($Variant -eq 'before'){$env:WOWEE_DISABLE_CONTACT_SHADOWS='1'}else{Remove-Item Env:WOWEE_DISABLE_CONTACT_SHADOWS -ErrorAction SilentlyContinue}
if($Legacy){$env:WOWEE_VK_NO_DYNAMIC_RENDERING='1'}else{Remove-Item Env:WOWEE_VK_NO_DYNAMIC_RENDERING -ErrorAction SilentlyContinue}
$env:WOWEE_SINGLE_THREAD_RECORD='1'
$env:WOWEE_CONFIG_ROOT="$root\build\$label-config"
$env:APPDATA=$env:WOWEE_CONFIG_ROOT
$env:WOWEE_RECORD="$root\build\$label.mp4"
$env:WOWEE_RECORD_SECONDS='7'
$env:WOWEE_LOG_FILE="$label.log"
$env:WOWEE_LOG_LEVEL='info'
$env:WOWEE_LOG_STDOUT='0'
$env:WOWEE_VULKAN_VALIDATION='1'
$env:VK_LAYER_PATH="$root\build\validation-runtime\mingw64\bin"
$env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'
$env:VK_LOADER_LAYERS_DISABLE='~implicit~'
New-Item -ItemType Directory -Force -Path $env:WOWEE_CONFIG_ROOT | Out-Null
Set-Content "$env:WOWEE_CONFIG_ROOT\settings.cfg" "graphics_preset=4`nantialiasing=3`nray_traced_lighting=1`nview_distance=2400`nfog_strength=1`nshadows=1`nshadow_distance=500`nnormal_mapping=1`nfov=70`nbrightness=50`nupscaling_mode=0`nfsr=0" -Encoding ASCII
$p=Start-Process "$runtime\wowee.exe" -WorkingDirectory $runtime -PassThru -RedirectStandardOutput "$root\build\$label.out" -RedirectStandardError "$root\build\$label.err"
$null=$p.Handle
Write-Output "$label PID=$($p.Id)"
if(-not $p.WaitForExit(180000)){throw "Fixture timeout PID=$($p.Id)"}
$p.Refresh()
if($p.ExitCode -ne 0){throw "Fixture exit=$($p.ExitCode)"}
$log=Get-Content "$runtime\logs\$label.log" -Raw
$map=if($Scene -in @('dalaran','sholazar')){571}else{0}
if($log -notmatch "Shadow scene: map=$map floor=" -or $log -notmatch ('hour=' + [regex]::Escape($env:WOWEE_SHADOW_PROBE_HOUR)) -or $log -notmatch 'Application exited successfully'){throw 'Scene/floor/hour verification failed'}
$errors=$log -split "`n" | Where-Object { $_ -match '\[ERROR|Validation Error|VUID-' -and $_ -notmatch 'loader_get_json: Failed to open JSON file .*EOSOverlayVkLayer-Win(32|64)\.json' }
if($errors){$errors | Select-Object -First 6 | Write-Output; throw 'Vulkan validation failed'}
Write-Output "$label passed: real map/floor, fixed hour, Vulkan validation, clean shutdown"
