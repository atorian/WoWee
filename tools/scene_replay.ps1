<#
.SYNOPSIS
Replay once and compare with saved metrics; optionally capture or run paired render modes.
.EXAMPLE
 .\tools\scene_replay.ps1 Capture -Recording D:\games\WoWee\build-dev\scratch\dalaran.json -Profile D:\path\to\active-profile -Data D:\games\WoWee\Data
.EXAMPLE
 .\tools\scene_replay.ps1
#>
param(
    [Parameter(Position=0)] [ValidateSet('Capture','Replay','AB','Benchmark')] [string]$Mode='Replay',
    [string]$Recording,
    [string]$Baseline,
    [string]$Profile,
    [string]$Data,
    [ValidateRange(1,300)] [int]$Seconds=30,
    [ValidateRange(1,120)] [int]$Settle=10,
    [ValidateSet(0,1)] [int]$RenderThread=1
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if (-not $Baseline) { $Baseline=Join-Path $root 'tasks\performance\dalaran-baseline.json' }
$reference=$null
if (Test-Path $Baseline) { $reference=Get-Content $Baseline -Raw -Encoding UTF8 | ConvertFrom-Json }
if ($Mode -eq 'Replay' -and $reference -and $reference.settleSeconds -and -not $PSBoundParameters.ContainsKey('Settle')) {
    $Settle=[int]$reference.settleSeconds
    if ($Settle -lt 1 -or $Settle -gt 120) { throw 'Saved baseline settleSeconds must be between 1 and 120.' }
}
if (-not $Recording -and ($Mode -eq 'Replay' -or $Mode -eq 'AB') -and $reference) { $Recording=$reference.scene }
if (-not $Recording) { throw 'Specify -Recording, or use Replay with a saved baseline containing the scene path.' }
$bin=Join-Path $root 'build-dev\bin'
$exe=Join-Path $bin 'wowee.exe'
$Recording=[System.IO.Path]::GetFullPath($Recording)
if ($Recording -notmatch '^[Dd]:\\') { throw 'Recording and reports must be on D:.' }
$work=Join-Path $root ('build-dev\scratch\scene-replay\run-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Force -Path $work | Out-Null
$env:TMP=$env:TEMP=$env:TMPDIR=Join-Path $root 'build-dev\tmp'
New-Item -ItemType Directory -Force -Path $env:TMP | Out-Null
$env:Path='C:\msys64\mingw64\bin;C:\msys64\usr\bin;'+$env:Path
$env:PATHEXT='.COM;.EXE;.BAT;.CMD'
$env:WOWEE_RESOURCE_ROOT=$bin
$env:WOWEE_LOG_LEVEL='info'
$env:WOWEE_VULKAN_VALIDATION='0'
$env:WOWEE_RENDER_THREAD="$RenderThread"
$env:PYTHONDONTWRITEBYTECODE='1'
foreach($name in @('WOWEE_SCENE_REPLAY','WOWEE_SCENE_CAPTURE','WOWEE_PERF_TRACE','WOWEE_BENCHMARK',
                  'WOWEE_GPU_MEMORY_DUMP','WOWEE_HEAP_SUMMARY','WOWEE_M2_CULL_CHECK',
                  'WOWEE_MEMORY_BY_OWNER','WOWEE_GPU_MEMORY_CHECK','WOWEE_FRAME_PROFILE')) {
    Remove-Item "Env:$name" -ErrorAction SilentlyContinue
}
$env:WOWEE_SCENE_REPLAY_SETTLE="$Settle"

function Start-Client([string]$LogName) {
    $env:WOWEE_LOG_FILE=$LogName
    & (Join-Path $root 'run.ps1') -NoProfile
    if (-not $?) { throw 'Client launcher failed' }
    $process=Get-Process wowee -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq $exe}
    if (-not $process) { throw 'Client exited during startup' }
    return $process
}
if ($Mode -eq 'Capture' -or $Mode -eq 'Benchmark') {
    if (-not $Profile -or -not (Test-Path $Profile)) { throw 'Capture/Benchmark requires the complete active -Profile.' }
    if (-not $Data) { throw 'Capture/Benchmark requires -Data (the root with expansions\).' }
    if (Test-Path $Recording) { throw "Capture exists: $Recording" }
    $env:WOWEE_CONFIG_ROOT=[System.IO.Path]::GetFullPath($Profile)
    $env:WOW_DATA_PATH=[System.IO.Path]::GetFullPath($Data)
    $env:WOWEE_SCENE_CAPTURE=$Recording
    $env:WOWEE_SCENE_CAPTURE_SECONDS="$Seconds"
    $env:WOWEE_PERF_TRACE=Join-Path $work 'capture.perf.json'
    if ($Mode -eq 'Benchmark') {
        $env:WOWEE_BENCHMARK='dalaran'
        $env:WOWEE_BENCHMARK_SETTLE='8'
        $env:WOWEE_BENCHMARK_SECONDS="$(20+$Seconds)"
    }
    $p=Start-Client 'scene-capture.log'
    Write-Host "Capture armed in client $($p.Id). Enter Dalaran once; recording starts after three seconds. Ctrl+F9 starts/stops manually."
    Write-Host "Recording: $Recording"
    if($Mode -eq 'Benchmark') { $p | Wait-Process -Timeout 300 }
    return
}

if (-not (Test-Path $Recording)) { throw "No recording: $Recording" }
$doc=Get-Content $Recording -Raw -Encoding UTF8 | ConvertFrom-Json
if ($doc.version -ne 1 -or -not $doc.complete -or $doc.frames.Count -eq 0) { throw 'Incomplete or unsupported scene recording.' }
$env:WOW_DATA_PATH=$doc.dataRoot
$env:WOWEE_SCENE_REPLAY=$Recording
$traces=@()
$modes=if($Mode -eq 'AB') {@(0,1)} else {@($RenderThread)}
foreach($thread in $modes) {
    $config=Join-Path $work "profile-$thread"
    New-Item -ItemType Directory -Path $config | Out-Null
    $utf8=New-Object System.Text.UTF8Encoding($false)
    foreach($name in @('settings.cfg','cvars.cfg')) {
        [System.IO.File]::WriteAllText((Join-Path $config $name),$doc.config.$name,$utf8)
    }
    # No account or server configuration is needed by replay. The actual
    # graphics settings and bound CVars come from the capture together.
    $env:WOWEE_CONFIG_ROOT=$config
    $env:WOWEE_RENDER_THREAD="$thread"
    $env:WOWEE_PERF_TRACE=Join-Path $work "thread-$thread.perf.json"
    $p=Start-Client "scene-replay-$thread.log"
    try { $p | Wait-Process -Timeout 600 }
    catch { $p | Stop-Process -Force; throw 'Replay did not finish within 10 minutes; inspect its log.' }
    if (-not (Test-Path $env:WOWEE_PERF_TRACE)) { throw "Replay produced no report; inspect scene-replay-$thread.log" }
    $traces+=$env:WOWEE_PERF_TRACE
}
$analysis=@((Join-Path $PSScriptRoot 'perf_trace.py'))+$traces
if ($Mode -eq 'Replay' -and $reference) {
    if ($reference.scene -eq $Recording) { $analysis+=@('--baseline',$Baseline) }
    else { Write-Warning 'Saved baseline belongs to a different scene; writing this run without a baseline comparison.' }
}
& python @analysis
if ($LASTEXITCODE -ne 0) { throw 'Replay trace validation failed' }
Write-Host "Reports: $work"
