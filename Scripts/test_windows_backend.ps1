# Exercises the Windows TerminalBackend over its named pipe: sessions, the
# WebSocket bridge, resuming, and sessions outliving the backend.
#
#   powershell -File Scripts\test_windows_backend.ps1 -BackendPath path\to\TerminalBackend.exe
#
# The backend's directory needs the page in a web folder next to it, and Outer Shell's own
# Terminal must not be running, since both use the same pipe.
param([Parameter(Mandatory)][string]$BackendPath)
$ErrorActionPreference = 'Stop'
$out = Split-Path -Parent (Resolve-Path $BackendPath)
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ClaudeJob {
    [StructLayout(LayoutKind.Sequential)] struct Basic { public long a; public long b; public uint LimitFlags; public UIntPtr c; public UIntPtr d; public uint e; public UIntPtr f; public uint g; public uint h; }
    [StructLayout(LayoutKind.Sequential)] struct Io { public ulong a, b, c, d, e, f; }
    [StructLayout(LayoutKind.Sequential)] struct Extended { public Basic Basic; public Io Io; public UIntPtr a, b, c, d; }
    [DllImport("kernel32.dll")] static extern IntPtr CreateJobObjectW(IntPtr a, IntPtr n);
    [DllImport("kernel32.dll")] static extern bool SetInformationJobObject(IntPtr j, int c, ref Extended i, int l);
    [DllImport("kernel32.dll")] static extern bool AssignProcessToJobObject(IntPtr j, IntPtr p);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    public static IntPtr Create(uint flags) { var j = CreateJobObjectW(IntPtr.Zero, IntPtr.Zero); var i = new Extended(); i.Basic.LimitFlags = flags; SetInformationJobObject(j, 9, ref i, Marshal.SizeOf(i)); return j; }
    public static bool Assign(IntPtr j, IntPtr p) { return AssignProcessToJobObject(j, p); }
}
'@
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value.ToLowerInvariant()
$pipeName = "org.outershell.terminal.$sid"
function Start-Backend { $p = Start-Process -FilePath "$out\TerminalBackend.exe" -PassThru; $null = $p.Handle; for ($i = 0; $i -lt 50 -and !(Test-Path "\\.\pipe\$pipeName"); $i++) { Start-Sleep -Milliseconds 100 }; $p }
function Open-Pipe { $c = New-Object IO.Pipes.NamedPipeClientStream('.', $pipeName, [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous); $c.Connect(5000); $c }
function Read-Head($s) { $b = New-Object Collections.Generic.List[byte]; while ($true) { $x = $s.ReadByte(); if ($x -lt 0) { break }; $b.Add([byte]$x); if ($b.Count -ge 4 -and $b[$b.Count-1] -eq 10 -and $b[$b.Count-2] -eq 13 -and $b[$b.Count-3] -eq 10 -and $b[$b.Count-4] -eq 13) { break } }; [Text.Encoding]::ASCII.GetString($b.ToArray()) }
function Http($method, $path) {
  $s = Open-Pipe; $req = [Text.Encoding]::ASCII.GetBytes("$method $path HTTP/1.1`r`nHost: localhost`r`nContent-Length: 0`r`n`r`n"); $s.Write($req, 0, $req.Length); $s.Flush()
  $head = Read-Head $s
  $length = if ($head -match 'Content-Length:\s*(\d+)') { [int]$Matches[1] } else { 0 }
  $body = New-Object byte[] $length; $read = 0
  while ($read -lt $length) { $n = $s.Read($body, $read, $length - $read); if ($n -le 0) { break }; $read += $n }
  $s.Dispose(); [pscustomobject]@{ Head = $head; Body = [Text.Encoding]::UTF8.GetString($body) } }
function Sessions { @((Http 'GET' '/api/sessions').Body -split "`n" | Select-Object -Skip 1 | Where-Object { $_ }) }
function Ws($path) {
  $s = Open-Pipe
  $req = [Text.Encoding]::ASCII.GetBytes("GET $path HTTP/1.1`r`nHost: localhost`r`nUpgrade: websocket`r`nConnection: Upgrade`r`nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==`r`nSec-WebSocket-Version: 13`r`n`r`n")
  $s.Write($req, 0, $req.Length); $s.Flush()
  $head = Read-Head $s
  [pscustomobject]@{ Stream = $s; Status = ($head -split ' ')[1]; Output = ''; Position = $null; Ended = $false; Pending = (New-Object Collections.Generic.List[byte]); Task = $null; Buffer = (New-Object byte[] 65536) } }
function Send($ws, [byte[]]$payload) {
  $frame = New-Object Collections.Generic.List[byte]; $frame.Add(0x82)
  if ($payload.Length -lt 126) { $frame.Add([byte](0x80 -bor $payload.Length)) } else { $frame.Add(0xFE); $frame.Add([byte]($payload.Length -shr 8)); $frame.Add([byte]($payload.Length -band 255)) }
  $mask = [byte[]](1,2,3,4); $frame.AddRange($mask)
  for ($i = 0; $i -lt $payload.Length; $i++) { $frame.Add([byte]($payload[$i] -bxor $mask[$i % 4])) }
  $bytes = $frame.ToArray(); $ws.Stream.Write($bytes, 0, $bytes.Length); $ws.Stream.Flush() }
function Type-Text($ws, $text) { Send $ws ([byte[]](@([byte][char]'I') + [Text.Encoding]::UTF8.GetBytes($text))) }
function Pump($ws, $pattern, $seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $deadline -and !$ws.Ended) {
    if ($pattern -and $ws.Output -match $pattern) { return $true }
    if (!$ws.Task) { $ws.Task = $ws.Stream.ReadAsync($ws.Buffer, 0, $ws.Buffer.Length) }
    if (!$ws.Task.Wait(300)) { continue }
    $n = $ws.Task.Result; $ws.Task = $null
    if ($n -le 0) { break }
    for ($i = 0; $i -lt $n; $i++) { $ws.Pending.Add($ws.Buffer[$i]) }
    while ($ws.Pending.Count -ge 2) {
      $len = $ws.Pending[1] -band 127; $off = 2
      if ($len -eq 126) { if ($ws.Pending.Count -lt 4) { break }; $len = ([int]$ws.Pending[2] -shl 8) + [int]$ws.Pending[3]; $off = 4 }
      elseif ($len -eq 127) { if ($ws.Pending.Count -lt 10) { break }; $len = 0; for ($k = 2; $k -lt 10; $k++) { $len = $len * 256 + [int]$ws.Pending[$k] }; $off = 10 }
      if ($ws.Pending.Count -lt $off + $len) { break }
      $op = $ws.Pending[0] -band 15
      $payload = $ws.Pending.GetRange($off, $len).ToArray(); $ws.Pending.RemoveRange(0, $off + $len)
      if ($op -eq 2 -and $payload.Length -gt 0) {
        $t = [char]$payload[0]
        if ($t -eq 'O') { $ws.Output += [Text.Encoding]::UTF8.GetString($payload, 1, $payload.Length - 1); if ($ws.Position -ne $null) { $ws.Position += $payload.Length - 1 } }
        elseif ($t -eq 'P') { $ws.Position = [BitConverter]::ToUInt64($payload, 1) }
        elseif ($t -eq 'X') { $ws.Ended = $true }
      }
    }
  }
  if ($pattern) { return [bool]($ws.Output -match $pattern) }
  return $false }
function Check($name, $condition) { if ($condition) { "ok   $name" } else { "FAIL $name"; $script:failed = $true } }
$failed = $false
$job = [ClaudeJob]::Create(0x2000 -bor 0x800)
$backend = Start-Backend
Check 'backend joins a kill-on-close job' ([ClaudeJob]::Assign($job, $backend.Handle))
try {
  $list = Http 'GET' '/api/sessions'
  Check 'lists sessions as TSV' ($list.Head -match '200 OK' -and $list.Body.StartsWith("id`tcommand"))
  Check 'offers PowerShell and Command Prompt' ($list.Head -match 'Terminal-Shells: powershell\tPowerShell\tcmd\tCommand Prompt')
  $before = (Sessions).Count
  $created = Http 'POST' '/api/sessions?shell=cmd&columns=100&rows=30'
  $id = $created.Body.Trim()
  Check 'starts a session' ($created.Head -match '201' -and $id.Length -eq 16)
  Check 'lists the new session' ((Sessions).Count -eq $before + 1)
  $ws = Ws "/api/sessions/$id/attach?columns=100&rows=30"
  Check 'attaches' ($ws.Status -eq '101')
  Check 'shows a prompt' (Pump $ws 'C:\\Users\\[^>]*>' 15)
  Type-Text $ws "echo first-%USERNAME:~0,0%visit`r"
  Check 'runs commands' (Pump $ws 'first-visit' 10)
  Pump $ws $null 1 | Out-Null
  Type-Text $ws "ping -n 4 127.0.0.1 >nul & echo missed-%USERNAME:~0,0%output`r"
  Pump $ws $null 1 | Out-Null
  $position = $ws.Position
  Check 'reports its position' ($position -ne $null)
  $ws.Stream.Dispose()
  Start-Sleep 5
  $resumed = Ws "/api/sessions/$id/attach?columns=100&rows=30&from=$position"
  Check 'resume delivers missed output' (Pump $resumed 'missed-output' 10)
  Check 'resume skips earlier output' ($resumed.Output -notmatch 'first-visit')
  $resumed.Stream.Dispose()
  [ClaudeJob]::CloseHandle($job) | Out-Null
  Start-Sleep 1
  Check 'closing the job stops the backend' ($backend.HasExited)
  $backend = Start-Backend
  Check 'session outlives the backend' (@(Sessions | Where-Object { $_.StartsWith($id) }).Count -eq 1)
  $again = Ws "/api/sessions/$id/attach?columns=100&rows=30"
  Check 'replay after restart' (Pump $again 'missed-output' 10)
  Type-Text $again "exit`r"
  Pump $again $null 4 | Out-Null
  Check 'exit ends the session' ($again.Ended -and @(Sessions | Where-Object { $_.StartsWith($id) }).Count -eq 0)
  $other = (Http 'POST' '/api/sessions?shell=powershell&columns=80&rows=24').Body.Trim()
  Start-Sleep 2
  $row = @(Sessions | Where-Object { $_.StartsWith($other) })[0]
  Check 'PowerShell session' ($row -match "`t(pwsh|powershell)`t")
  $closed = Http 'POST' "/api/sessions/$other/close"
  Start-Sleep 1
  Check 'close ends a session' ($closed.Head -match '204' -and @(Sessions | Where-Object { $_.StartsWith($other) }).Count -eq 0)
} finally {
  foreach ($session in @($id, $other)) {
    if ($session) { try { Http 'POST' "/api/sessions/$session/close" | Out-Null } catch {} }
  }
  if (!$backend.HasExited) { Stop-Process -Id $backend.Id -Force }
}
if ($failed) { 'some checks failed' } else { 'all passed' }
