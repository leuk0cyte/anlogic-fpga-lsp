# =============================================================================
#  tools/perf/reports/run.ps1 —— 报告管线探针的采集驱动
# =============================================================================
#  做三件事（README §6 要求）：
#    1. 跑前/跑后各采一次系统负载（GetSystemTimes 差分），写进日志；
#    2. 用 perf_reports.exe 跑指定场景，输出到 tools/perf/reports/out/；
#    3. 记录探针 exe 的时间戳与 build.ninja 的 /O2 计数（构建可信度声明）。
#
#  用法：
#     pwsh -File tools\perf\reports\run.ps1 -Scene all -Iters 15 -Rounds 3
#     pwsh -File tools\perf\reports\run.ps1 -Scene hit -Iters 15
# =============================================================================
param(
    [string]$Scene = "all",
    [int]$Iters = 15,
    [int]$ColdIters = 5,
    [int]$Rounds = 1,
    [string]$Project = "",
    [string]$Exe = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$outDir = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

if (-not $Exe) { $Exe = Join-Path $repo 'build\bin\perf_reports.exe' }
if (-not $Project) {
    $Project = Join-Path $env:TEMP 'fpgalsp-perf\reports\report100\report100.al'
}
if (-not (Test-Path $Exe)) { throw "探针不存在: $Exe（先 pwsh -File tools\perf\build.ps1 -Target perf_reports）" }
if (-not (Test-Path $Project)) { throw "合成工程不存在: $Project（先 python tools/perf/reports/make_synth.py）" }

$seriesRoot = Split-Path -Parent (Split-Path -Parent $Project)

function Get-LoadSample {
    $sig = @'
using System;
using System.Runtime.InteropServices;
public static class CpuTimes {
    [StructLayout(LayoutKind.Sequential)] public struct FILETIME { public uint dwLowDateTime; public uint dwHighDateTime; }
    [DllImport("kernel32.dll")] public static extern bool GetSystemTimes(out FILETIME idle, out FILETIME kernel, out FILETIME user);
    public static ulong[] Sample() {
        FILETIME idle, kernel, user;
        GetSystemTimes(out idle, out kernel, out user);
        return new ulong[] {
            ((ulong)idle.dwHighDateTime << 32) | idle.dwLowDateTime,
            ((ulong)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime,
            ((ulong)user.dwHighDateTime << 32) | user.dwLowDateTime };
    }
}
'@
    if (-not ('CpuTimes' -as [type])) { Add-Type -TypeDefinition $sig | Out-Null }
    return [CpuTimes]::Sample()
}

$stamp = Get-Item $Exe
$o2 = (Select-String -Path (Join-Path $repo 'build\build.ninja') -Pattern '/O2' -AllMatches |
       Measure-Object).Count

$log = Join-Path $outDir ("run-{0}-{1}.log" -f $Scene, (Get-Date -Format 'HHmmss'))
"=== perf_reports driver ===" | Tee-Object -FilePath $log
"scene=$Scene iters=$Iters cold_iters=$ColdIters rounds=$Rounds" | Tee-Object -FilePath $log -Append
"exe=$Exe" | Tee-Object -FilePath $log -Append
"exe_LastWriteTime=$($stamp.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')) bytes=$($stamp.Length)" |
    Tee-Object -FilePath $log -Append
"build_ninja_O2_count=$o2" | Tee-Object -FilePath $log -Append
"project=$Project" | Tee-Object -FilePath $log -Append

for ($round = 1; $round -le $Rounds; $round++) {
    $before = Get-LoadSample
    $t0 = Get-Date
    "--- round $round start $($t0.ToString('HH:mm:ss')) ---" | Tee-Object -FilePath $log -Append
    & $Exe $Scene --al $Project --iters $Iters --cold-iters $ColdIters --series-root $seriesRoot 2>&1 |
        Tee-Object -FilePath $log -Append
    $wall = ((Get-Date) - $t0).TotalSeconds
    $after = Get-LoadSample
    $idle = [double]($after[0] - $before[0])
    $total = [double](($after[1] - $before[1]) + ($after[2] - $before[2]))
    $busy = if ($total -gt 0) { 100.0 * (1.0 - $idle / $total) } else { 0.0 }
    ("load: busy={0:N1}% wall={1:N1}s (window {2:N1}s)" -f $busy, $wall, $wall) |
        Tee-Object -FilePath $log -Append
}
"done -> $log"