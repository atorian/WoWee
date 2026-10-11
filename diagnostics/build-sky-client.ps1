$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
Set-Location 'D:\games\WoWee\build'
$skyNinja = [System.IO.File]::ReadAllText('D:\games\WoWee\build\build.ninja')
$skyNinja = $skyNinja.Replace('bin/wowee.exe', 'bin/wowee-sky-fix.exe').Replace('bin\wowee.exe', 'bin\wowee-sky-fix.exe')
$skyNinja = $skyNinja.Replace('CMakeFiles/wowee.rsp', 'CMakeFiles/wowee-sky.rsp').Replace('CMakeFiles\wowee.rsp', 'CMakeFiles\wowee-sky.rsp')
$skyNinja = $skyNinja.Replace('  LINK_FLAGS = -Wno-odr', '  LINK_FLAGS = -Wno-odr -flto=4')
[System.IO.File]::WriteAllText('D:\games\WoWee\build\sky-fix.ninja', $skyNinja)
& 'C:\msys64\mingw64\bin\ninja.exe' -f sky-fix.ninja -j 4 wowee
exit $LASTEXITCODE
