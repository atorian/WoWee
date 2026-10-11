$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
Set-Location 'D:\games\WoWee\build'
$terrainNinja = [IO.File]::ReadAllText('D:\games\WoWee\build\build.ninja')
$terrainNinja = $terrainNinja.Replace('bin/wowee.exe', 'bin/wowee-terrain-fix.exe').Replace('bin\wowee.exe', 'bin\wowee-terrain-fix.exe')
$terrainNinja = $terrainNinja.Replace('CMakeFiles/wowee.rsp', 'CMakeFiles/wowee-terrain.rsp').Replace('CMakeFiles\wowee.rsp', 'CMakeFiles\wowee-terrain.rsp')
$terrainNinja = $terrainNinja.Replace('  LINK_FLAGS = -Wno-odr', '  LINK_FLAGS = -Wno-odr -flto=4')
[IO.File]::WriteAllText('D:\games\WoWee\build\terrain-fix.ninja', $terrainNinja)
& 'C:\msys64\mingw64\bin\ninja.exe' -f terrain-fix.ninja -j 4 wowee test_adt_alpha test_adt_truncation test_grass_terrain test_m2_sway
exit $LASTEXITCODE
