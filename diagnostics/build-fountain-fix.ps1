$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
Set-Location 'D:\games\WoWee'
& 'C:\msys64\mingw64\bin\cmake.exe' --build build --target test_m2_material test_m2_blend_mode test_m2_sky_material --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Material test build failed' }
foreach ($test in @('test_m2_material', 'test_m2_blend_mode', 'test_m2_sky_material')) {
  & ".\build\bin\$test.exe"
  if ($LASTEXITCODE -ne 0) { throw "Regression failed: $test" }
}
Set-Location 'D:\games\WoWee\build'
$fountainNinja = [System.IO.File]::ReadAllText('D:\games\WoWee\build\build.ninja')
$fountainNinja = $fountainNinja.Replace('bin/wowee.exe', 'bin/wowee-fountain-fix.exe').Replace('bin\wowee.exe', 'bin\wowee-fountain-fix.exe')
$fountainNinja = $fountainNinja.Replace('CMakeFiles/wowee.rsp', 'CMakeFiles/wowee-fountain.rsp').Replace('CMakeFiles\wowee.rsp', 'CMakeFiles\wowee-fountain.rsp')
$fountainNinja = $fountainNinja.Replace('  LINK_FLAGS = -Wno-odr', '  LINK_FLAGS = -Wno-odr -flto=4')
[System.IO.File]::WriteAllText('D:\games\WoWee\build\fountain-fix.ninja', $fountainNinja)
& 'C:\msys64\mingw64\bin\ninja.exe' -f fountain-fix.ninja -j 4 wowee
exit $LASTEXITCODE
