$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
$env:WOWEE_LOG_FILE = 'wowee-rt-halo-probe.log'
Set-Location 'D:\games\WoWee\build\bin'
$fixture = 'assets\shaders\rt_lookup_probe.comp.spv'
try {
  Remove-Item Env:WOWEE_RT_FORCE_SOFTWARE -ErrorAction SilentlyContinue
  Copy-Item '..\rt-halo-before\rt_lookup_probe.comp.spv' $fixture -Force
  $before = & '.\rt-halo-probe.exe' 2>&1
  $beforeCode = $LASTEXITCODE
  @($before) | Set-Content '..\rt-halo-before.out'
  $before | Where-Object { $_ -match '^lookup|^PASS|^FAIL' } | Write-Output
  $lookupFailures = @($before | Where-Object { $_ -match '^lookup.*FAIL' }).Count
  if ($beforeCode -ne 1 -or $lookupFailures -ne 5) { throw "Unexpected baseline: exit=$beforeCode lookup failures=$lookupFailures" }
  Copy-Item '..\rt-halo-shaders\rt_lookup_probe.comp.spv' $fixture -Force
  foreach ($backend in @('hardware', 'compute')) {
    if ($backend -eq 'compute') { $env:WOWEE_RT_FORCE_SOFTWARE = '1' } else { Remove-Item Env:WOWEE_RT_FORCE_SOFTWARE -ErrorAction SilentlyContinue }
    foreach ($location in @('origin', 'translated')) {
      $probeArgs = @()
      if ($location -eq 'translated') { $probeArgs += '--translated' }
      $output = & '.\rt-halo-probe.exe' @probeArgs 2>&1
      $code = $LASTEXITCODE
      $output | Set-Content "..\rt-halo-$backend-$location.out"
      Write-Output "$backend/$location exit=$code"
      $output | Where-Object { $_ -match '^lookup|^PASS|^FAIL|^unoccluded wall:|^backend:' } | Write-Output
      if ($code -ne 0) { throw "Regression failed: $backend/$location" }
    }
  }
} finally {
  Copy-Item '..\rt-halo-shaders\rt_lookup_probe.comp.spv' $fixture -Force
}
exit 0
