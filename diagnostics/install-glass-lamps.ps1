$ErrorActionPreference = 'Stop'
$root = 'D:\games\WoWee'
$bin = "$root\build\bin"
$target = "$bin\wowee.exe"
$running = Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq $target }
if ($running) { throw "Primary client is running (PID=$($running.ProcessId)); close it before replacement" }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = "$root\build\glass-lamps-backup-$stamp"
New-Item -ItemType Directory -Force -Path "$backup\runtime-shaders","$backup\source-shaders" | Out-Null
Copy-Item $target "$backup\wowee.exe" -Force
$names = @('character.frag','terrain.frag','wmo.frag','m2.frag','rt_trace.comp','rt_trace_hw.comp')
foreach ($name in $names) {
    Copy-Item "$bin\assets\shaders\$name.spv" "$backup\runtime-shaders\$name.spv" -Force
    Copy-Item "$root\assets\shaders\$name.spv" "$backup\source-shaders\$name.spv" -Force
}
Copy-Item "$bin\wowee-glass-lamps-validated.exe" $target -Force
foreach ($name in $names) {
    Copy-Item "$root\build\glass-lamps-shaders\$name.spv" "$root\assets\shaders\$name.spv" -Force
    Copy-Item "$root\build\glass-lamps-shaders\$name.spv" "$bin\assets\shaders\$name.spv" -Force
    if ((Get-FileHash "$root\assets\shaders\$name.spv").Hash -ne (Get-FileHash "$bin\assets\shaders\$name.spv").Hash) { throw "Shader mismatch: $name" }
}
if ((Get-FileHash $target).Hash -ne (Get-FileHash "$bin\wowee-glass-lamps-validated.exe").Hash) { throw 'Executable mismatch' }
Write-Output "Installed validated client and six matching shaders. Backup: $backup"
