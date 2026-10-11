$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
Set-Location 'D:\games\WoWee'
& 'C:\msys64\mingw64\bin\cmake.exe' --build build --target test_light_bands --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Light/fog test build failed' }
& '.\build\bin\test_light_bands.exe'
if ($LASTEXITCODE -ne 0) { throw 'Light/fog regression failed' }
Set-Location 'D:\games\WoWee\build'
$fogNinja = [System.IO.File]::ReadAllText('D:\games\WoWee\build\build.ninja')
$fogNinja = $fogNinja.Replace('bin/wowee.exe', 'bin/wowee-distance-fog.exe').Replace('bin\wowee.exe', 'bin\wowee-distance-fog.exe')
$fogNinja = $fogNinja.Replace('CMakeFiles/wowee.rsp', 'CMakeFiles/wowee-distance-fog.rsp').Replace('CMakeFiles\wowee.rsp', 'CMakeFiles\wowee-distance-fog.rsp')
$fogNinja = $fogNinja.Replace('  LINK_FLAGS = -Wno-odr', '  LINK_FLAGS = -Wno-odr -flto=4')
[System.IO.File]::WriteAllText('D:\games\WoWee\build\distance-fog.ninja', $fogNinja)
& 'C:\msys64\mingw64\bin\ninja.exe' -f distance-fog.ninja -j 4 wowee
exit $LASTEXITCODE
