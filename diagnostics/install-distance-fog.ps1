$ErrorActionPreference = 'Stop'
$root = 'D:\games\WoWee'
$sourceExe = "$root\build\bin\wowee-distance-fog.exe"
$installedExe = "$root\build\bin\wowee.exe"
if ((Get-Item $sourceExe).Length -lt 1000000) { throw 'Client build is incomplete' }
foreach ($shader in @('m2.frag', 'character.frag', 'terrain.frag', 'wmo.frag', 'grass.frag', 'water.frag', 'skybox.frag')) {
  $authored = (Get-FileHash "$root\assets\shaders\$shader.spv" -Algorithm SHA256).Hash
  $runtime = (Get-FileHash "$root\build\bin\assets\shaders\$shader.spv" -Algorithm SHA256).Hash
  if ($authored -ne $runtime) { throw "Runtime shader differs: $shader" }
}
& "$root\diagnostics\distance-fog-smoke.ps1"
if ($LASTEXITCODE -ne 0) { throw 'Startup validation failed' }
[System.IO.File]::Copy($sourceExe, $installedExe, $true)
$sourceHash = (Get-FileHash $sourceExe -Algorithm SHA256).Hash
if ((Get-FileHash $installedExe -Algorithm SHA256).Hash -ne $sourceHash) { throw 'Installed client hash differs' }
Write-Output ('Installed verified client: ' + (Get-Item $installedExe).Length + ' bytes, SHA256=' + $sourceHash)
