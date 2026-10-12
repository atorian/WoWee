$ErrorActionPreference = 'Stop'
$root = 'D:\games\WoWee\build\bin'
$source = "$root\wowee-mulgore-fix.exe"
$target = "$root\wowee.exe"
$backup = "$root\wowee-before-mulgore-fix-20261011.exe"
if ((Get-Item $source).Length -lt 1000000) { throw 'Client build is incomplete' }
$log = Get-Content "$root\logs\mulgore-validated.log" -Raw
if ($log -notmatch 'Mulgore probe: world entry completed' -or $log -notmatch 'Entity stream probe: completed players=24 ' -or $log -notmatch 'Application exited successfully') { throw 'Candidate has not passed the Mulgore fixture' }
$errors = $log -split "`n" | Where-Object { $_ -match '\[ERROR|Validation Error|VUID-' -and $_ -notmatch 'loader_get_json: Failed to open JSON file .*EOSOverlayVkLayer-Win(32|64)\.json' }
if ($errors) { throw 'Validated log contains errors' }
$hash = (Get-FileHash $source -Algorithm SHA256).Hash
if (Test-Path $backup) { throw 'Backup already exists; inspect before replacing' }
[System.IO.File]::Move($target, $backup)
try {
    [System.IO.File]::Copy($source, $target)
    if ((Get-FileHash $target -Algorithm SHA256).Hash -ne $hash) { throw 'Installed hash differs' }
} catch {
    if (Test-Path $target) { [System.IO.File]::Delete($target) }
    [System.IO.File]::Move($backup, $target)
    throw
}
Write-Output "Installed Mulgore fix: SHA256=$hash"
Write-Output "Previous client: $backup"
