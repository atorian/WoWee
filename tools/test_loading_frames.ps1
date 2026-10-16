param(
    [Parameter(Mandatory = $true)][string]$ProfileSource,
    [string]$BuildDirectory = "build-dev",
    [string[]]$Cases = @("close", "resize", "disconnect", "logout", "transfer", "release"),
    [uint32]$MapId = 1,
    [float]$SpawnX = -2917.58, [float]$SpawnY = -257.98, [float]$SpawnZ = 52.9968,
    [ValidateSet("warmup", "geometry", "preparing")][string]$Stage = "warmup"
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$build = (Resolve-Path "$root\$BuildDirectory").Path
# Built beside wowee.exe in every configuration with tests: the same client
# objects with the fixture's own main (tests/loading_test_client_main.cpp).
$executable = "$build\bin\wowee_loading_test_client.exe"
if (-not (Test-Path $executable)) {
    throw "Build it first: ninja -C $BuildDirectory wowee_loading_test_client; missing $executable"
}
$source = (Resolve-Path $ProfileSource).Path
$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
$env:PATHEXT = ".COM;.EXE;.BAT;.CMD"
$env:TMP = "$build\tmp"
$env:TEMP = $env:TMP
$env:TMPDIR = $env:TMP
New-Item -ItemType Directory -Force $env:TMP | Out-Null
Remove-Item Env:WOWEE_SCREENSHOT -ErrorAction SilentlyContinue
Remove-Item Env:WOWEE_RECORD -ErrorAction SilentlyContinue
$env:WOWEE_MULGORE_PROBE = "1"
$env:WOWEE_LOG_LEVEL = "info"
$env:WOWEE_WORLD_PROBE_MAP = "$MapId"
$culture = [System.Globalization.CultureInfo]::InvariantCulture
$env:WOWEE_WORLD_PROBE_X = $SpawnX.ToString($culture)
$env:WOWEE_WORLD_PROBE_Y = $SpawnY.ToString($culture)
$env:WOWEE_WORLD_PROBE_Z = $SpawnZ.ToString($culture)
$env:WOWEE_LOADING_TEST_STAGE = $Stage
foreach ($case in $Cases) {
    $label = if ($MapId -eq 1 -and $Stage -eq "warmup") { $case } else { "$case-$MapId-$Stage" }
    $run = "$build\scratch\loading-$label"
    $env:WOWEE_CONFIG_ROOT = "$run-config"
    $env:APPDATA = "$run-appdata"
    $env:LOCALAPPDATA = "$run-localappdata"
    New-Item -ItemType Directory -Force $env:WOWEE_CONFIG_ROOT | Out-Null
    # Preserve the whole profile: realms, character/account choices and graphics.
    Copy-Item "$source\*" $env:WOWEE_CONFIG_ROOT -Recurse -Force
    $env:WOWEE_LOADING_TEST = $case
    $process = Start-Process $executable -WorkingDirectory "$build\bin" -PassThru `
        -RedirectStandardOutput "$run.out" -RedirectStandardError "$run.err"
    # Keep its native handle before waiting: Windows PowerShell otherwise
    # sometimes loses ExitCode when it notices the process has already exited.
    $processHandle = $process.Handle
    if (-not $process.WaitForExit(120000)) {
        Stop-Process -Id $process.Id
        throw "Loading scenario $case did not terminate within 120 seconds; see $run.out"
    }
    $process.Refresh()
    $log = Get-Content "$run.out" -Raw
    if ($process.ExitCode -ne 0 -or $log -match "Loading probe FAIL|requires phase" -or
        $log -notmatch "Loading probe PASS: $case") {
        throw "Loading scenario $case failed; exit=$($process.ExitCode), see $run.out and $run.err"
    }
    ($log -split "`n" | Where-Object { $_ -match "Loading probe PASS:" }) | Write-Output
}
