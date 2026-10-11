$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
Set-Location 'D:\games\WoWee'
& 'C:\msys64\mingw64\bin\cmake.exe' --build build --target test_m2_sky_material --parallel 4
exit $LASTEXITCODE
