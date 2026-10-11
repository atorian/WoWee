$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WoweeCpuSample {
 [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access,bool inherit,uint id);
 [DllImport("kernel32.dll")] static extern uint SuspendThread(IntPtr t);
 [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr t);
 [DllImport("kernel32.dll")] static extern bool GetThreadContext(IntPtr t,IntPtr context);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 public static long Instruction(uint id) {
  IntPtr t=OpenThread(0x1a,false,id); if(t==IntPtr.Zero)throw new Exception("OpenThread failed");
  IntPtr raw=Marshal.AllocHGlobal(1250); IntPtr c=new IntPtr((raw.ToInt64()+15)&~15L); bool suspended=false;
  try {
   for(int i=0;i<1232;i++)Marshal.WriteByte(c,i,0);
   Marshal.WriteInt32(c,48,0x100001);
   if(SuspendThread(t)==uint.MaxValue)throw new Exception("SuspendThread failed"); suspended=true;
   if(!GetThreadContext(t,c))throw new Exception("GetThreadContext failed");
   return Marshal.ReadInt64(c,248);
  } finally { if(suspended)ResumeThread(t); Marshal.FreeHGlobal(raw);CloseHandle(t); }
 }
}
'@
$client = Get-Process -Name wowee-fast-login -ErrorAction Stop
if ($client.Path -ne 'D:\games\WoWee\build\bin\wowee-fast-login.exe') { throw 'Unexpected client path' }
$main = $client.Threads | Sort-Object StartTime | Select-Object -First 1
$base = $client.MainModule.BaseAddress.ToInt64()
$counts = @{}
for ($n=0; $n -lt 200; $n++) {
 $ip = [WoweeCpuSample]::Instruction([uint32]$main.Id)
 $key = if ($ip -ge $base -and $ip -lt ($base + $client.MainModule.ModuleMemorySize)) { 'rva:0x{0:x}' -f ($ip-$base) } else { 'outside-image' }
 $counts[$key] = 1 + $counts[$key]
 Start-Sleep -Milliseconds 7
}
Write-Output ('Thread={0} Base=0x{1:x} Samples=200' -f $main.Id,$base)
$counts.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 25 | ForEach-Object { Write-Output ('{0} {1}' -f $_.Key,$_.Value) }
