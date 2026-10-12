$ErrorActionPreference = 'Stop'
$root = 'D:\games\WoWee\build\bin'
$source = "$root\wowee-world-socket-fix.exe"
$target = "$root\wowee.exe"
$backup = "$root\wowee-before-world-socket-fix.exe"
if ((Get-Item $source).Length -lt 1000000) { throw 'Client build is incomplete' }
$sourceHash = (Get-FileHash $source -Algorithm SHA256).Hash
if (Test-Path $backup) { throw 'Backup already exists; inspect before replacing it' }
try {
  [System.IO.File]::Move($target, $backup)
} catch [System.IO.IOException] {
  Write-Output "Current executable is locked. Use the validated client directly: $source"
  exit 2
}
try {
  [System.IO.File]::Copy($source, $target, $true)
  if ((Get-FileHash $target -Algorithm SHA256).Hash -ne $sourceHash) { throw 'Installed hash differs' }
} catch {
  if (Test-Path $target) { [System.IO.File]::Delete($target) }
  [System.IO.File]::Move($backup, $target)
  throw
}
Write-Output "Installed verified network fix: SHA256=$sourceHash"
Write-Output "Previous executable preserved: $backup"
exit 0
