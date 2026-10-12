param([ValidateSet('configure','build','test','noop')][string]$Phase = 'configure')
$ErrorActionPreference = 'Stop'
$env:PATHEXT = '.COM;.EXE;.BAT;.CMD'
$env:Path = 'C:\msys64\mingw64\bin;C:\msys64\usr\bin;' + $env:Path
Set-Location 'D:\games\WoWee'
$cmake = 'C:\msys64\mingw64\bin\cmake.exe'
$ctest = 'C:\msys64\mingw64\bin\ctest.exe'
$timer = [Diagnostics.Stopwatch]::StartNew()
switch ($Phase) {
    'configure' {
        & $cmake --preset dev -B build/pr38 -DCMAKE_BUILD_TYPE=Release -DWOWEE_BUILD_EDITOR=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1 | Tee-Object build/pr38-configure.log
    }
    'build' {
        & $cmake --build build/pr38 --parallel 4 2>&1 | Tee-Object build/pr38-build.log
    }
    'test' {
        & $ctest --test-dir build/pr38 --parallel 4 --output-on-failure 2>&1 | Tee-Object build/pr38-tests.log
    }
    'noop' {
        & $cmake --build build/pr38 --parallel 4 2>&1 | Tee-Object build/pr38-noop.log
    }
}
$result = $LASTEXITCODE
$timer.Stop()
Write-Output "PR38 $Phase exit=$result elapsed=$($timer.Elapsed.TotalSeconds)s"
exit $result
