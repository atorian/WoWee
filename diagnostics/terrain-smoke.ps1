$ErrorActionPreference = 'Stop'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
$env:WOWEE_CONFIG_ROOT = 'D:\games\WoWee\build\terrain-smoke-config'
$env:WOW_DATA_PATH = 'D:\games\WoWee\Data'
$env:WOWEE_LOG_FILE = 'wowee-terrain-smoke.log'
$env:WOWEE_LOG_LEVEL = 'info'
$env:WOWEE_LOG_STDOUT = '0'
$env:WOWEE_SCREENSHOT = 'D:\games\WoWee\build\terrain-smoke.png'
New-Item -ItemType Directory -Force -Path $env:WOWEE_CONFIG_ROOT | Out-Null
$smokeLog = 'D:\games\WoWee\build\bin\logs\wowee-terrain-smoke.log'
Remove-Item $env:WOWEE_SCREENSHOT, $smokeLog -Force -ErrorAction SilentlyContinue
$p = Start-Process -FilePath 'D:\games\WoWee\build\bin\wowee-terrain-fix.exe' -WorkingDirectory 'D:\games\WoWee\build\bin' -PassThru -RedirectStandardOutput 'D:\games\WoWee\build\terrain-smoke.out' -RedirectStandardError 'D:\games\WoWee\build\terrain-smoke.err'
if (-not $p.WaitForExit(45000)) { throw ('Smoke test still running; PID=' + $p.Id) }
if ($null -ne $p.ExitCode -and $p.ExitCode -ne 0) { throw ('Smoke exit=' + $p.ExitCode) }
if (-not (Test-Path $env:WOWEE_SCREENSHOT)) { throw 'Screenshot was not produced' }
$logText = Get-Content $smokeLog -Raw
if ($logText -notmatch 'Application exited successfully' -or $logText -match '\[ERROR') { throw 'Startup log did not confirm a clean exit' }
foreach ($shader in @('terrain.frag','m2.vert','shadow.vert')) {
  $authoredHash = (Get-FileHash ("D:\games\WoWee\assets\shaders\$shader.spv") -Algorithm SHA256).Hash
  $runtimeHash = (Get-FileHash ("D:\games\WoWee\build\bin\assets\shaders\$shader.spv") -Algorithm SHA256).Hash
  if ($authoredHash -ne $runtimeHash) { throw "Runtime shader differs: $shader" }
}
Write-Output 'Smoke passed: screenshot produced, clean shutdown, runtime shaders match source SPIR-V'
