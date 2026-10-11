$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
Set-Location 'D:\games\WoWee\build\bin'
$results = @()
foreach ($backend in @('hardware', 'compute')) {
  if ($backend -eq 'compute') { $env:WOWEE_RT_FORCE_SOFTWARE = '1' } else { Remove-Item Env:WOWEE_RT_FORCE_SOFTWARE -ErrorAction SilentlyContinue }
  foreach ($location in @('origin', 'translated')) {
    $probeArgs = @()
    if ($location -eq 'translated') { $probeArgs += '--translated' }
    $output = & '.\rt_probe.exe' @probeArgs 2>&1
    $code = $LASTEXITCODE
    $output | Set-Content "..\rt-artifact-$backend-$location.out"
    $output | Where-Object { $_ -match 'camera:|wall |unoccluded wall:|trace [0-9]|PASS|FAIL|backend:' } | Write-Output
    $results += "$backend/$location exit=$code"
  }
}
$results | Write-Output
if ($results -match 'exit=[1-9]') { exit 1 }
exit 0
