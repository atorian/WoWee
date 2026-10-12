$ErrorActionPreference = 'Stop'
$root = 'D:\games\WoWee'
$client = "$root\build\bin\wowee-fast-session.exe"
if (-not (Test-Path $client) -or (Get-Item $client).Length -lt 1000000) { throw 'Fast session build is not ready' }
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
$env:WOWEE_LOG_FILE = 'fast-session.log'
$env:WOWEE_LOG_LEVEL = 'info'
$env:WOWEE_LOG_STDOUT = '0'
$process = Start-Process $client -WorkingDirectory "$root\build\bin" -PassThru -RedirectStandardOutput "$root\build\fast-session-runtime.out" -RedirectStandardError "$root\build\fast-session-runtime.err"
Write-Output "Started fast session test client: PID=$($process.Id)"
Write-Output 'Sign in manually; timing is recorded in build/bin/logs/fast-session.log'
