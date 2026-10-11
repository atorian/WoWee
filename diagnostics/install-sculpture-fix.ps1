$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
$root = 'D:\games\WoWee'
$sourceExe = "$root\build\bin\wowee-sculpture-fix.exe"
$installedExe = "$root\build\bin\wowee.exe"
if ((Get-Item $sourceExe).Length -lt 1000000) { throw 'Client build is incomplete' }
foreach ($shader in @('m2.vert', 'm2.frag')) {
  & 'C:\msys64\mingw64\bin\spirv-val.exe' --target-env vulkan1.0 "$root\assets\shaders\$shader.spv"
  if ($LASTEXITCODE -ne 0) { throw "Invalid shader: $shader" }
}
foreach ($shader in @('m2.vert', 'm2.frag', 'character.frag', 'terrain.frag', 'wmo.frag', 'grass.frag', 'water.frag', 'skybox.frag')) {
  $authored = (Get-FileHash "$root\assets\shaders\$shader.spv" -Algorithm SHA256).Hash
  $runtime = (Get-FileHash "$root\build\bin\assets\shaders\$shader.spv" -Algorithm SHA256).Hash
  if ($authored -ne $runtime) { throw "Runtime shader differs: $shader" }
}
& "$root\diagnostics\sculpture-fix-smoke.ps1"
if ($LASTEXITCODE -ne 0) { throw 'Startup validation failed' }
$activePrimary = Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq $installedExe }
if ($activePrimary) { throw 'Primary client is running; verified build is available as wowee-sculpture-fix.exe' }
[System.IO.File]::Copy($sourceExe, $installedExe, $true)
$sourceHash = (Get-FileHash $sourceExe -Algorithm SHA256).Hash
if ((Get-FileHash $installedExe -Algorithm SHA256).Hash -ne $sourceHash) { throw 'Installed client hash differs' }
Write-Output ('Installed verified client: ' + (Get-Item $installedExe).Length + ' bytes, SHA256=' + $sourceHash)
