$ErrorActionPreference = 'Stop'
$root = 'D:\games\WoWee'
$bin = "$root\build\bin"
$source = "$bin\wowee-orgrimmar-light.exe"
$target = "$bin\wowee.exe"
if (-not (Test-Path $source) -or (Get-Item $source).Length -lt 1000000) { throw 'Validated Orgrimmar build missing' }
$running = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq $target })
if ($running.Count) { throw 'Primary client is open; close it before replacement' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = "$bin\wowee-before-orgrimmar-$stamp.exe"
Copy-Item $target $backup
Copy-Item $source $target -Force
if ((Get-FileHash $source).Hash -ne (Get-FileHash $target).Hash) { throw 'Installed checksum mismatch' }
Write-Output "Installed Orgrimmar daylight fix. Backup: $backup"
