<#
.SYNOPSIS
    perf-profiler 的 ETW 采集脚本（**需要管理员 PowerShell**）。

.DESCRIPTION
    本会话（Medium integrity，未提权）无法启动 NT Kernel Logger，实测：
        xperf -on PROC_THREAD+LOADER+PROFILE  -> NT Kernel Logger: 无效标志 (0x3ec)
        wpr   -start CPU                      -> Failed to enable the policy to profile system performance
    CPU 采样需要 SeSystemProfilePrivilege（只授予管理员），因此这条路径只能由有权限的
    用户在**提权的 PowerShell** 里跑一次。产出 CSV 后，本仓库的 summarize 流程可以直接消费
    （表名映射见同目录 README.md）。

    用法（管理员 PowerShell）：
        cd Y:\fpga-lsp-cpp
        .\tools\perf\profiler\collect_etw.ps1 -Scenario all -Exe build-prof\bin\fpgalsp-native.exe

    产物：%TEMP%\fpgalsp-perf\profiler\etw\<scenario>.etl
          %TEMP%\fpgalsp-perf\profiler\etw\<scenario>\*.csv（CPU 采样按进程/线程/栈聚合）
          最后复制到 tools/perf/profiler/out/etw/（<5 MB 的部分）

.NOTES
    * 采样会全机生效；脚本在采集前后各记一次系统负载（GetSystemTimes）写进 etw/load.csv。
    * 只用 xperf + wpaexporter（PerfView 不可用）；wpaexporter 必须配 .wpaprofile，
      本脚本会生成一个最小的 profile（CPU Usage (Sampled) 表）。
#>
[CmdletBinding()]
param(
    [ValidateSet('a', 'b', 'c', 'd', 'all')]
    [string]$Scenario = 'all',
    [string]$Exe = 'build-prof\bin\fpgalsp-native.exe',
    [string]$Root = "$env:TEMP\fpgalsp-perf\profiler\proj200",
    [int]$IntervalMs = 1,
    [int]$MinMs = 8000
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$wpt = 'C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit'
$xperf = Join-Path $wpt 'xperf.exe'
$exporter = Join-Path $wpt 'wpaexporter.exe'
$outDir = Join-Path $env:TEMP 'fpgalsp-perf\profiler\etw'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# ---- 0) 权限自检 -------------------------------------------------------------
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).
    IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Error "需要管理员 PowerShell（当前 Medium integrity）。本会话无法启动 NT Kernel Logger，见脚本头部说明。"
    exit 1
}

# ---- 1) 系统负载（采集前后各一次） -------------------------------------------
function Get-CpuSnapshot {
    Add-Type -Name 'K32' -Namespace 'PerfEtw' -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
'@ -ErrorAction SilentlyContinue
    $idle = 0L; $kernel = 0L; $user = 0L
    [void][PerfEtw.K32]::GetSystemTimes([ref]$idle, [ref]$kernel, [ref]$user)
    return [pscustomobject]@{ idle = $idle; kernel = $kernel; user = $user }
}

# ---- 2) 最小 wpaexporter profile（CPU Usage (Sampled)） ----------------------
$profilePath = Join-Path $outDir 'cpu-sampled.wpaprofile'
@'
<?xml version="1.0" encoding="utf-8"?>
<Profile Version="1.0">
  <ExportTable Name="CPU Usage (Sampled)">
    <Column Name="Process" />
    <Column Name="Thread ID" />
    <Column Name="Stack" />
    <Column Name="Count" />
    <Column Name="Weight" />
  </ExportTable>
</Profile>
'@ | Set-Content -Path $profilePath -Encoding utf8

$scenarios = if ($Scenario -eq 'all') { @('a', 'b', 'c', 'd') } else { @($Scenario) }
$loadRows = @()

foreach ($s in $scenarios) {
    $etl = Join-Path $outDir "$s.etl"
    $csvDir = Join-Path $outDir $s
    New-Item -ItemType Directory -Force -Path $csvDir | Out-Null
    Write-Host "[etw] scenario $s -> $etl"

    $before = Get-CpuSnapshot
    # 采样：CPU 采样 + 线程/进程/模块事件 + 调用栈
    & $xperf -on PROC_THREAD+LOADER+PROFILE -stackwalk Profile -buffersize 1024 -minbuffers 256 -maxbuffers 512
    if ($LASTEXITCODE -ne 0) { Write-Error "xperf -on 失败（$LASTEXITCODE）"; exit 2 }

    try {
        $args = @{
            exe    = Join-Path $repo $Exe
            root   = $Root
            scenario = $s
            out    = Join-Path $outDir 'runs'
            interval = $IntervalMs
        }
        if ($s -eq 'd') { $args.min = 6000 } elseif ($s -ne 'a') { $args.min = $MinMs }
        $pythonArgs = @('tools\perf\profiler\scenario_driver.py', '--exe', $args.exe, '--root', $args.root,
                        '--scenario', $args.scenario, '--out', $args.out, '--interval-ms', $args.interval)
        if ($args.min) { $pythonArgs += @('--min-ms', "$($args.min)") }
        Push-Location $repo
        try { & python @pythonArgs } finally { Pop-Location }
    }
    finally {
        & $xperf -d $etl
    }
    $after = Get-CpuSnapshot
    $dIdle = $after.idle - $before.idle
    $dTotal = ($after.kernel - $before.kernel) + ($after.user - $before.user)
    $busy = if ($dTotal -gt 0) { [math]::Round(100.0 * ($dTotal - $dIdle) / $dTotal, 1) } else { 0 }
    $loadRows += [pscustomobject]@{ scenario = $s; busy_pct = $busy; etl = $etl }
    Write-Host "[etw] scenario $s done (busy $busy%)"

    # 导出 CSV
    & $exporter -i $etl -outputfolder $csvDir -profile $profilePath 2>&1 | Out-String | Write-Verbose
    if ($LASTEXITCODE -ne 0) { Write-Warning "wpaexporter 失败（$LASTEXITCODE），ETL 保留在 $etl" }
}

$loadRows | Export-Csv -Path (Join-Path $outDir 'load.csv') -NoTypeInformation -Encoding utf8
Write-Host "[etw] 全部完成。ETL/CSV 在 $outDir"
Write-Host "[etw] 把需要入库的 CSV 复制到 tools\perf\profiler\out\etw\（保持 <5 MB）"