$ErrorActionPreference = 'Stop'
$bin = 'D:\games\WoWee\build\bin'
$source = "$bin\wowee-early-character-list.exe"
$target = "$bin\wowee.exe"
if (-not (Test-Path $source) -or (Get-Item $source).Length -lt 1000000) { throw 'Validated early query build missing' }
$running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $target })
if ($running.Count) { throw 'Primary client is running; close it before installation' }
$backup = "$bin\wowee-before-early-character-list-20261012.exe"
if (-not (Test-Path $backup)) { Copy-Item $target $backup }
Copy-Item $source $target -Force
if ((Get-FileHash $source).Hash -ne (Get-FileHash $target).Hash) { throw 'Installed executable checksum mismatch' }
Write-Output 'Installed early character list client as wowee.exe; previous executable backed up'
