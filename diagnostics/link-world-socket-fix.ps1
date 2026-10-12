$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
Set-Location 'D:\games\WoWee\build'
$sourceResponse = [System.IO.File]::ReadAllText('D:\games\WoWee\build\CMakeFiles\wowee-world-socket.rsp')
$snapshot = 'CMakeFiles/wowee-world-socket-snapshot.dir/'
$objects = [regex]::Matches($sourceResponse, 'CMakeFiles/wowee\.dir/[^ ]+\.obj')
foreach ($object in $objects) {
  $source = $object.Value
  $target = $source.Replace('CMakeFiles/wowee.dir/', $snapshot)
  New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
  Copy-Item $source $target -Force
  if ((Get-FileHash $source).Hash -ne (Get-FileHash $target).Hash) { throw "Object changed during snapshot: $source" }
}
$snapshotResponse = $sourceResponse.Replace('CMakeFiles/wowee.dir/', $snapshot)
[System.IO.File]::WriteAllText('D:\games\WoWee\build\CMakeFiles\wowee-world-socket-snapshot.rsp', $snapshotResponse)
Write-Output "Linking immutable snapshot of $($objects.Count) object files"
& 'C:\msys64\mingw64\bin\c++.exe' -O3 -DNDEBUG -flto=4 -fno-fat-lto-objects -Wno-odr '@CMakeFiles\wowee-world-socket-snapshot.rsp' -o 'bin\wowee-world-socket-fix.exe' '-Wl,--out-implib,lib\libwowee-world-socket.dll.a' '-Wl,--major-image-version,0,--minor-image-version,0'
if ($LASTEXITCODE -ne 0) { throw 'Snapshot link failed' }
& 'C:\msys64\mingw64\bin\cmake.exe' -E copy_directory 'D:/games/WoWee/assets' 'D:/games/WoWee/build/bin/assets'
if ($LASTEXITCODE -ne 0) { throw 'Asset sync failed' }
Write-Output 'Client linked successfully'
exit 0
