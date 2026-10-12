$ErrorActionPreference = 'Stop'
$root = 'D:\games\WoWee\build\bin'
$source = "$root\wowee-entity-streaming.exe"
$target = "$root\wowee.exe"
$backup = "$root\wowee-before-entity-streaming-$([DateTime]::Now.ToString('yyyyMMdd-HHmmss')).exe"
if ((Get-Item $source).Length -lt 1000000) { throw 'Client build is incomplete' }
foreach ($variant in @('batched', 'batched-validated', 'batched-validated-expanded')) {
    $logPath = "$root\logs\entity-streaming-$variant.log"
    $log = Get-Content $logPath -Raw
    if ($log -notmatch 'Entity stream probe: completed players=24 ' -or
        $log -notmatch 'Application exited successfully') { throw "Incomplete GPU probe: $logPath" }
    $errors = $log -split "`n" | Where-Object {
        $_ -match '\[ERROR|Validation Error|VUID-' -and
        $_ -notmatch 'loader_get_json: Failed to open JSON file .*EOSOverlayVkLayer-Win(32|64)\.json'
    }
    if ($errors) { throw "Failed GPU probe: $logPath" }
    if ((Get-Item $source).LastWriteTime -gt (Get-Item $logPath).LastWriteTime) {
        throw 'Executable changed after its probe'
    }
}
$sourceHash = (Get-FileHash $source -Algorithm SHA256).Hash
if ((Get-FileHash $target -Algorithm SHA256).Hash -eq $sourceHash) {
    Write-Output "Already installed: SHA256=$sourceHash"
    exit 0
}
$stage = "$source.installing"
if (Test-Path $stage) { throw 'Staged installer file already exists; inspect it before retrying' }
[System.IO.File]::Copy($source, $stage, $false)
if ((Get-FileHash $stage -Algorithm SHA256).Hash -ne $sourceHash) { throw 'Staged hash differs' }
try {
    [System.IO.File]::Move($target, $backup)
} catch [System.IO.IOException] {
    [System.IO.File]::Delete($stage)
    Write-Output "Current executable is locked. Verified client: $source"
    exit 2
}
try {
    [System.IO.File]::Move($stage, $target)
} catch {
    if (-not (Test-Path $target)) { [System.IO.File]::Move($backup, $target) }
    throw
}
Write-Output "Installed background entity streaming: SHA256=$sourceHash"
Write-Output "Previous executable preserved: $backup"
exit 0
