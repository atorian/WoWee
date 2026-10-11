$ErrorActionPreference = 'Stop'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
Set-Location 'D:\games\WoWee\build'
foreach ($test in @('test_adt_alpha','test_adt_truncation','test_grass_terrain','test_m2_sway')) {
  $testOut = "D:\games\WoWee\build\$test.out"
  $testErr = "D:\games\WoWee\build\$test.err"
  $p = Start-Process -FilePath "D:\games\WoWee\build\bin\$test.exe" -WorkingDirectory 'D:\games\WoWee\build\bin' -Wait -PassThru -RedirectStandardOutput $testOut -RedirectStandardError $testErr
  Get-Content $testOut
  Get-Content $testErr
  if ($p.ExitCode -ne 0) { throw "Test failed: $test exit=$($p.ExitCode)" }
}
