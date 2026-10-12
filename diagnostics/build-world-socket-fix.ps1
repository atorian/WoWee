$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
Set-Location 'D:\games\WoWee\build'
$socketNinja = [System.IO.File]::ReadAllText('D:\games\WoWee\build\build.ninja')
$socketNinja = $socketNinja.Replace('bin/wowee.exe', 'bin/wowee-world-socket-fix.exe').Replace('bin\wowee.exe', 'bin\wowee-world-socket-fix.exe')
$socketNinja = $socketNinja.Replace('CMakeFiles/wowee.rsp', 'CMakeFiles/wowee-world-socket.rsp').Replace('CMakeFiles\wowee.rsp', 'CMakeFiles\wowee-world-socket.rsp')
$socketNinja = $socketNinja.Replace('  LINK_FLAGS = -Wno-odr', '  LINK_FLAGS = -Wno-odr -flto=4')
# Separate Ninja journals avoid a restat/recompaction conflict with another build.
$state = 'D:\games\WoWee\build\world-socket-state'
New-Item -ItemType Directory -Force -Path $state | Out-Null
foreach ($journal in @('.ninja_log', '.ninja_deps')) {
  if (-not (Test-Path "$state\$journal")) { Copy-Item ".\$journal" "$state\$journal" }
}
$socketNinja = "builddir = world-socket-state`n" + $socketNinja
[System.IO.File]::WriteAllText('D:\games\WoWee\build\world-socket-fix.ninja', $socketNinja)
& 'C:\msys64\mingw64\bin\ninja.exe' -f world-socket-fix.ninja -j 4 test_world_socket
if ($LASTEXITCODE -ne 0) { throw 'World socket test build failed' }
& '.\bin\test_world_socket.exe'
if ($LASTEXITCODE -ne 0) { throw 'World socket regression failed' }
& 'C:\msys64\mingw64\bin\ninja.exe' -f world-socket-fix.ninja -j 4 wowee
exit $LASTEXITCODE
