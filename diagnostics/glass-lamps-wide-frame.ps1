$ErrorActionPreference='Stop'
$env:PATHEXT='.COM;.EXE;.BAT;.CMD'
$env:Path='C:\msys64\mingw64\bin;'+$env:Path
$root='D:\games\WoWee'
Copy-Item 'C:\msys64\mingw64\bin\ffmpeg.exe' "$root\build\glass-lamps-runtime\ffmpeg.exe" -Force
$p=Start-Process "$root\build\glass-lamps-runtime\ffmpeg.exe" -WorkingDirectory "$root\build\glass-lamps-runtime" -ArgumentList @('-y','-ss','0.5','-i',"$root\build\glass-lamps-scene-wide-0-0.mp4",'-frames:v','1',"$root\build\glass-lamps-wide.png") -PassThru -WindowStyle Hidden -RedirectStandardOutput "$root\build\glass-lamps-wide-frame.out" -RedirectStandardError "$root\build\glass-lamps-wide-frame.err"
$null=$p.Handle
$p.WaitForExit()
$p.Refresh()
Write-Output "ffmpeg exit=$($p.ExitCode)"
if ($p.ExitCode -ne 0) { throw 'Frame extraction failed' }
