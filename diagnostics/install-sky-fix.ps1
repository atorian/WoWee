$ErrorActionPreference = 'Stop'
$sourceExe = 'D:\games\WoWee\build\bin\wowee-sky-fix.exe'
$installedExe = 'D:\games\WoWee\build\bin\wowee.exe'
if ((Get-Item $sourceExe).Length -lt 1000000) { throw 'Client build is incomplete' }
[System.IO.File]::Copy($sourceExe, $installedExe, $true)
$sourceHash = (Get-FileHash $sourceExe -Algorithm SHA256).Hash
if ((Get-FileHash $installedExe -Algorithm SHA256).Hash -ne $sourceHash) { throw 'Installed client hash differs' }
foreach ($shader in @('m2.vert','m2.frag','character.frag','terrain.frag','wmo.frag','rt_prepare.comp','rt_prepare_ms.comp','rt_trace.comp','rt_trace_hw.comp','rt_temporal.comp')) {
  $authoredHash = (Get-FileHash ("D:\games\WoWee\assets\shaders\$shader.spv") -Algorithm SHA256).Hash
  $runtimeHash = (Get-FileHash ("D:\games\WoWee\build\bin\assets\shaders\$shader.spv") -Algorithm SHA256).Hash
  if ($authoredHash -ne $runtimeHash) { throw "Runtime shader differs: $shader" }
}
Write-Output ('Installed verified client: ' + (Get-Item $installedExe).Length + ' bytes, SHA256=' + $sourceHash)
& 'D:\games\WoWee\diagnostics\sky-smoke.ps1'
exit $LASTEXITCODE
