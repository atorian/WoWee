$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
Set-Location 'D:\games\WoWee\build'
$streamNinja = [System.IO.File]::ReadAllText('D:\games\WoWee\build\build.ninja')
$streamNinja = $streamNinja.Replace('bin/wowee.exe', 'bin/wowee-entity-streaming.exe').Replace('bin\wowee.exe', 'bin\wowee-entity-streaming.exe')
$streamNinja = $streamNinja.Replace('CMakeFiles/wowee.rsp', 'CMakeFiles/wowee-entity-streaming.rsp').Replace('CMakeFiles\wowee.rsp', 'CMakeFiles\wowee-entity-streaming.rsp')
$streamNinja = $streamNinja.Replace('  LINK_FLAGS = -Wno-odr', '  LINK_FLAGS = -Wno-odr -flto=4')
[System.IO.File]::WriteAllText('D:\games\WoWee\build\entity-streaming.ninja', $streamNinja)
& 'C:\msys64\mingw64\bin\ninja.exe' -f entity-streaming.ninja -j 4 wowee
exit $LASTEXITCODE
