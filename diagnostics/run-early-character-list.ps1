$ErrorActionPreference = 'Stop'
$root = 'D:\games\WoWee'
$client = "$root\build\bin\wowee-early-character-list.exe"
if (-not (Test-Path $client) -or (Get-Item $client).Length -lt 1000000) { throw 'Early character list build is not ready' }
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
$env:WOWEE_LOG_FILE = 'early-character-list.log'
$env:WOWEE_LOG_LEVEL = 'info'
$env:WOWEE_LOG_STDOUT = '0'
$process = Start-Process $client -WorkingDirectory "$root\build\bin" -PassThru -RedirectStandardOutput "$root\build\early-character-list-runtime.out" -RedirectStandardError "$root\build\early-character-list-runtime.err"
Write-Output "Started early character list test client: PID=$($process.Id)"
Write-Output 'Sign in manually; timing is recorded in build/bin/logs/early-character-list.log'
