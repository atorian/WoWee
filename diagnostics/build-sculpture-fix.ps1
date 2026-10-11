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
$sculptureNinja = [System.IO.File]::ReadAllText('D:\games\WoWee\build\build.ninja')
$sculptureNinja = $sculptureNinja.Replace('bin/wowee.exe', 'bin/wowee-sculpture-fix.exe').Replace('bin\wowee.exe', 'bin\wowee-sculpture-fix.exe')
$sculptureNinja = $sculptureNinja.Replace('CMakeFiles/wowee.rsp', 'CMakeFiles/wowee-sculpture.rsp').Replace('CMakeFiles\wowee.rsp', 'CMakeFiles\wowee-sculpture.rsp')
$sculptureNinja = $sculptureNinja.Replace('  LINK_FLAGS = -Wno-odr', '  LINK_FLAGS = -Wno-odr -flto=4')
[System.IO.File]::WriteAllText('D:\games\WoWee\build\sculpture-fix.ninja', $sculptureNinja)
& 'C:\msys64\mingw64\bin\ninja.exe' -f sculpture-fix.ninja -j 4 wowee
exit $LASTEXITCODE
