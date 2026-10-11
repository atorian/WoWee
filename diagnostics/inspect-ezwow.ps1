$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class EzRuntimeRead {
 [DllImport("kernel32.dll",SetLastError=true)] public static extern IntPtr OpenProcess(uint access,bool inherit,int pid);
 [DllImport("kernel32.dll",SetLastError=true)] public static extern bool ReadProcessMemory(IntPtr process,IntPtr address,byte[] buffer,UIntPtr size,out UIntPtr read);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
 [DllImport("psapi.dll",SetLastError=true)] public static extern bool EnumProcessModulesEx(IntPtr process,IntPtr[] modules,uint size,out uint needed,uint flags);
 [DllImport("psapi.dll",CharSet=CharSet.Unicode)] public static extern uint GetModuleFileNameExW(IntPtr process,IntPtr module,System.Text.StringBuilder path,uint size);
}
'@
$clients = @(Get-Process -Name Wow -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq 'D:\games\Isengard_WotLK_335a\Wow.exe' })
foreach ($client in $clients) {
 Write-Output ('CLIENT PID={0} FILE={1}' -f $client.Id,$client.Path)
 Write-Output ('MODULE COUNT={0}' -f $client.Modules.Count)
 $client.Modules | Select-Object -First 8 -ExpandProperty FileName | ForEach-Object { Write-Output $_ }
 $dlls = @($client.Modules | Where-Object { $_.ModuleName -match 'ezLib|Awesome|Scan|Battle.net' } | ForEach-Object { '{0} BASE=0x{1:x} SIZE={2}' -f $_.FileName,$_.BaseAddress.ToInt64(),$_.ModuleMemorySize })
 $dlls | ForEach-Object { Write-Output $_ }
 $handle = [EzRuntimeRead]::OpenProcess(0x410,$false,$client.Id)
 if ($handle -eq [IntPtr]::Zero) { throw 'Cannot open client for read-only diagnostics' }
 try {
  $modules = New-Object IntPtr[] 2048
  [uint32]$needed = 0
  if ([EzRuntimeRead]::EnumProcessModulesEx($handle,$modules,16384,[ref]$needed,1)) {
   foreach ($module in $modules[0..([int]($needed / 8) - 1)]) {
    $name = New-Object Text.StringBuilder 1024
    [EzRuntimeRead]::GetModuleFileNameExW($handle,$module,$name,1024) | Out-Null
    if ($name.ToString() -match 'ezLib|Awesome|Scan|Battle.net') { Write-Output ('X86 DLL={0} BASE=0x{1:x}' -f $name.ToString(),$module.ToInt64()) }
   }
  }
  $buffer = New-Object byte[] 216
  [UIntPtr]$received = [UIntPtr]::Zero
  if (-not [EzRuntimeRead]::ReadProcessMemory($handle,[IntPtr]0xdd1000,$buffer,[UIntPtr]::new([uint32]216),[ref]$received)) { throw 'Cannot read extension region' }
  $path = 'D:\games\WoWee\diagnostics\ezwow-runtime-' + $client.Id + '.bin'
  [IO.File]::WriteAllBytes($path,$buffer)
  $hash = [Security.Cryptography.SHA256]::Create()
  $hex = ([BitConverter]::ToString($hash.ComputeHash($buffer))).Replace('-','').ToLowerInvariant()
  Write-Output ('REGION BYTES={0} NONZERO={1} SHA256={2} SAVED={3}' -f $received.ToUInt64(),(@($buffer | Where-Object { $_ -ne 0 }).Count),$hex,$path)
 } finally { [EzRuntimeRead]::CloseHandle($handle) | Out-Null }
}
