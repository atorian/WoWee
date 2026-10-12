$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
Set-Location 'D:\games\WoWee\build'
& 'C:\msys64\mingw64\bin\ninja.exe' -f world-socket-fix.ninja -j 4 test_world_socket
if ($LASTEXITCODE -ne 0) { throw 'World socket test build failed' }
& '.\bin\test_world_socket.exe'
exit $LASTEXITCODE
