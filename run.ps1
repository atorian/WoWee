<#
.SYNOPSIS
    Starts the client built in this checkout, with what it needs to log in.

.DESCRIPTION
    Runs build-dev\bin\wowee.exe (build\bin with -Release) from its own folder,
    so its logs\ and cache\ land beside it. Before starting it:

    - closes a wowee.exe already running from that same folder;
    - puts the MinGW runtime first on PATH (the client's DLLs, avformat and
      the rest, live there and nowhere else);
    - points TMP/TEMP at build-dev\tmp, off the system drive;
    - sets WOW_DATA_PATH to the game data, unless it is set: Data\ of the main
      checkout, which a git worktree shares rather than copies;
    - sets WOWEE_SERVER_EXTENSIONS_DIR to that data's server_extensions\,
      unless it is set. Without its profile an extension server such as EZWoW
      gets zeros for its Warden check of the runtime bootstrap, sends it again
      and again, and drops the connection after character select.

.PARAMETER Release
    Start build\bin (Release, LTO) instead of build-dev\bin.

.PARAMETER Build
    Build the client with ninja first; stop if the build fails.

.PARAMETER NoProfile
    Leave WOWEE_FRAME_PROFILE unset. By default it is 1, so the log carries
    the frame budget every 10 s.

.EXAMPLE
    .\run.ps1 -Build
#>
param(
    [switch]$Release,
    [switch]$Build,
    [switch]$NoProfile
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $ScriptDir $(if ($Release) { "build" } else { "build-dev" })
$binDir = Join-Path $buildDir "bin"
$exe = Join-Path $binDir "wowee.exe"

$mingw = "C:\msys64\mingw64\bin"
if (-not (Test-Path $mingw)) { throw "MinGW runtime not found at $mingw; the client cannot load its DLLs." }
$env:PATH = "$mingw;$env:PATH"
if ($env:PATHEXT -notmatch '\.EXE') { $env:PATHEXT = ".COM;.EXE;.BAT;.CMD;$env:PATHEXT" }

$tmp = Join-Path $ScriptDir "build-dev\tmp"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$env:TMP = $tmp; $env:TEMP = $tmp; $env:TMPDIR = $tmp

if ($Build) {
    & ninja -C $buildDir wowee
    if ($LASTEXITCODE -ne 0) { throw "Build failed in $buildDir; the client was not started." }
}
if (-not (Test-Path $exe)) { throw "$exe does not exist; build it first (.\run.ps1 -Build)." }

if (-not $env:WOW_DATA_PATH) {
    # The main checkout is the parent of the common .git directory; for the
    # main checkout itself that is this folder.
    $commonGit = & git -C $ScriptDir rev-parse --path-format=absolute --git-common-dir
    if ($LASTEXITCODE -ne 0) { throw "git could not say where the main checkout is; set WOW_DATA_PATH." }
    $env:WOW_DATA_PATH = Join-Path (Split-Path -Parent $commonGit) "Data"
}
if (-not (Test-Path (Join-Path $env:WOW_DATA_PATH "expansions"))) {
    throw "No game data at $env:WOW_DATA_PATH (no expansions\ in it); set WOW_DATA_PATH."
}
if (-not $env:WOWEE_SERVER_EXTENSIONS_DIR) {
    $env:WOWEE_SERVER_EXTENSIONS_DIR = Join-Path $env:WOW_DATA_PATH "server_extensions"
}
if (-not $NoProfile -and -not $env:WOWEE_FRAME_PROFILE) { $env:WOWEE_FRAME_PROFILE = "1" }

$running = Get-Process wowee -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
if ($running) {
    Write-Host "Closing the running client ($($running.Id -join ', '))"
    $running | Stop-Process -Force
    $running | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
}

$process = Start-Process -FilePath $exe -WorkingDirectory $binDir -PassThru
Write-Host "Started $exe (pid $($process.Id))"
Write-Host "  data:       $env:WOW_DATA_PATH"
Write-Host "  extensions: $env:WOWEE_SERVER_EXTENSIONS_DIR"
Write-Host "  log:        $(Join-Path $binDir 'logs\wowee.log')"
