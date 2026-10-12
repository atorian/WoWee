$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;' + $env:Path
$root = 'D:\games\WoWee'
$env:WOWEE_LOG_FILE = 'rt-glass-lamps-probe.log'
$env:WOWEE_VULKAN_VALIDATION = '1'
$env:VK_LAYER_PATH = "$root\build\validation-runtime\mingw64\bin"
$env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
$runtime = "$root\build\glass-lamps-runtime"
New-Item -ItemType Directory -Force -Path "$runtime\assets\shaders" | Out-Null
Copy-Item "$root\build\bin\*.dll" $runtime -Force
Copy-Item "$root\build\bin\rt-glass-lamps-probe.exe" $runtime -Force
Copy-Item "$root\build\bin\assets\shaders\*" "$runtime\assets\shaders" -Force
Copy-Item "$root\build\glass-lamps-shaders\*" "$runtime\assets\shaders" -Force
Set-Location $runtime
try {
    foreach ($backend in @('hardware','compute')) {
        if ($backend -eq 'compute') { $env:WOWEE_RT_FORCE_SOFTWARE='1' } else { Remove-Item Env:WOWEE_RT_FORCE_SOFTWARE -ErrorAction SilentlyContinue }
        foreach ($location in @('origin','translated')) {
            $probeArgs = @()
            if ($location -eq 'translated') { $probeArgs += '--translated' }
            $output = & '.\rt-glass-lamps-probe.exe' @probeArgs 2>&1
            $code = $LASTEXITCODE
            $output | Set-Content "$root\build\glass-lamps-$backend-$location.out"
            $output | Where-Object { $_ -match '^lookup|^PASS|^FAIL|^offscreen|^  mirror|^unoccluded|^backend' } | Write-Output
            if ($code -ne 0) { throw "GPU regression failed: $backend/$location, exit=$code" }
            $errors = $output | Where-Object { $_ -match '\[ERROR|Validation Error|VUID-' -and $_ -notmatch 'loader_get_json: Failed to open JSON file .*EOSOverlayVkLayer-Win(32|64)\.json' }
            if ($errors) { $errors | Write-Output; throw "GPU validation failed: $backend/$location" }
        }
    }
} finally {
    Remove-Item Env:WOWEE_RT_FORCE_SOFTWARE -ErrorAction SilentlyContinue
}
