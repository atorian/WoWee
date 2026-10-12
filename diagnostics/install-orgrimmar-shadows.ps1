$ErrorActionPreference = 'Stop'
$root = 'D:\games\WoWee'
$bin = "$root\build\bin"
$source = "$bin\wowee-orgrimmar-shadows.exe"
$target = "$bin\wowee.exe"
if (-not (Test-Path $source) -or (Get-Item $source).Length -lt 1000000) { throw 'Validated shadow client missing' }
$running = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq $target })
if ($running.Count) { throw "Primary client is open (PID=$($running.ProcessId)); close it before replacement" }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = "$root\build\orgrimmar-shadow-backup-$stamp"
New-Item -ItemType Directory -Force -Path $backup | Out-Null
Copy-Item $target "$backup\wowee.exe"
Copy-Item "$bin\assets\shaders\wmo.frag.spv" "$backup\runtime-wmo.frag.spv"
Copy-Item "$root\assets\shaders\wmo.frag.spv" "$backup\source-wmo.frag.spv"
Copy-Item $source $target -Force
Copy-Item "$root\build\orgrimmar-shadow-shaders\wmo.frag.spv" "$bin\assets\shaders\wmo.frag.spv" -Force
Copy-Item "$root\build\orgrimmar-shadow-shaders\wmo.frag.spv" "$root\assets\shaders\wmo.frag.spv" -Force
if ((Get-FileHash $source).Hash -ne (Get-FileHash $target).Hash) { throw 'Executable checksum mismatch' }
if ((Get-FileHash "$bin\assets\shaders\wmo.frag.spv").Hash -ne (Get-FileHash "$root\assets\shaders\wmo.frag.spv").Hash) { throw 'Shader checksum mismatch' }
Write-Output "Installed receiver-shadow client and matching WMO shader. Backup: $backup"
