$ErrorActionPreference = 'Stop'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
$env:WOWEE_CONFIG_ROOT = 'D:\games\WoWee\build\world-socket-fix-smoke-config'
$env:WOW_DATA_PATH = 'D:\games\WoWee\Data'
$env:WOWEE_LOG_FILE = 'wowee-world-socket-fix-smoke.log'
$env:WOWEE_LOG_LEVEL = 'info'
$env:WOWEE_LOG_STDOUT = '0'
$env:WOWEE_SCREENSHOT = 'D:\games\WoWee\build\world-socket-fix-smoke.png'
New-Item -ItemType Directory -Force -Path $env:WOWEE_CONFIG_ROOT | Out-Null
$smokeLog = 'D:\games\WoWee\build\bin\logs\wowee-world-socket-fix-smoke.log'
Remove-Item $env:WOWEE_SCREENSHOT, $smokeLog -Force -ErrorAction SilentlyContinue
$p = Start-Process -FilePath 'D:\games\WoWee\build\bin\wowee-world-socket-fix.exe' -WorkingDirectory 'D:\games\WoWee\build\bin' -PassThru -RedirectStandardOutput 'D:\games\WoWee\build\world-socket-fix-smoke.out' -RedirectStandardError 'D:\games\WoWee\build\world-socket-fix-smoke.err'
if (-not $p.WaitForExit(45000)) { throw ('Smoke test still running; PID=' + $p.Id) }
if ($null -ne $p.ExitCode -and $p.ExitCode -ne 0) { throw ('Smoke exit=' + $p.ExitCode) }
if (-not (Test-Path $env:WOWEE_SCREENSHOT)) { throw 'Screenshot was not produced' }
$logText = Get-Content $smokeLog -Raw
if ($logText -notmatch 'Application exited successfully' -or $logText -match '\[ERROR') { throw 'Startup log did not confirm a clean exit' }
Write-Output 'Smoke passed: screenshot produced and clean shutdown logged'
exit 0
