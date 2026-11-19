#!/usr/bin/env pwsh
# =============================================================================
#  tools/perf/gate/verify.ps1 —— F0 独立验收：一条命令跑完全部口径一致的测量
# =============================================================================
#  用途：给 F1–F6 提供统一的 before/after。**不采信 owner 自报的数字。**
#
#  每条命令都落一份 JSON：
#     audit/2026-11-19/fix/results/<Tag>.json            汇总（含 vs baseline 的对比与判定）
#     audit/2026-11-19/fix/results/<Tag>/*.json          各子测量的原始输出
#
#  用法：
#     pwsh -File tools\perf\gate\verify.ps1 -Tag wave1-f1
#     pwsh -File tools\perf\gate\verify.ps1 -Tag wave1-final -Rounds 3 -Samples 20
#     pwsh -File tools\perf\gate\verify.ps1 -Tag quick -Only latency        # 只跑一段
#     pwsh -File tools\perf\gate\verify.ps1 -Tag ab -Exe <某个旧 exe 的绝对路径>
#
#  纪律（见 audit/2026-11-19/fix/PLAN.md）：
#    * 只读 src/ 与 vscode-extension/，本脚本不构建、不改产品代码；
#    * 不覆盖 build/bin/fpgalsp-native.exe；用 -Exe 切换二进制时走 FPGALSP_NATIVE_EXE；
#    * 每项 ≥3 轮 × 20 样本；系统负载前后各记一次。
# =============================================================================
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [int]$Rounds = 3,
    [int]$Samples = 20,
    [string[]]$Sizes = @("200", "1000"),
    # 允许 -Only latency,protocol：pwsh -File 会把它合成一个字符串，下面统一拆分校验
    [string[]]$Only = @("all"),
    [string]$Exe = "",
    [int]$ReportMb = 10,
    [switch]$WithVscode,
    [switch]$NoProtocol,
    # 协议快照跑几遍（Lead 要求 ADC/SDC 四项每 tag 至少 3 遍）
    [int]$ProtocolRuns = 3,
    # 仅用于自检：模拟"测量期间源 exe 被重编"，验证作废路径真的会触发
    [switch]$SimulateExeChange,
    # -Only unit 时必填：要**先重编**再跑的测试目标，如 unit_index_service
    # （Lead 踩过的坑：跑的是上一个版本的测试二进制）
    [string[]]$UnitTarget = @(),
    # qor suggestions 的 step 顺序归一化。默认**关闭**（Lead 要求先有基线证据再默许）
    [switch]$NormalizeQorSuggestions
)

$ErrorActionPreference = 'Stop'
# $PSScriptRoot = <repo>\tools\perf\gate → 往上三层才是仓库根
$repo = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
Set-Location $repo

# 归一化 -Only：pwsh -File 会把 "-Only latency,reports" 合成一个字符串，这里拆开再校验。
# （不做这步会静默地一个阶段都不跑 —— 比报错更危险。）
$allowed = @("all", "latency", "phase2", "index", "protocol", "vscode", "reports", "unit")
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($stage in $Only) {
    if ($allowed -notcontains $stage) { throw "-Only 不支持 '$stage'（可选：$($allowed -join ' / ')）" }
}

$resultsDir = Join-Path $repo "audit\2026-11-19\fix\results"
$tagDir = Join-Path $resultsDir $Tag
$baselineDir = Join-Path $repo "audit\2026-11-19\fix\baseline"
New-Item -ItemType Directory -Force -Path $resultsDir, $tagDir | Out-Null

function Want([string]$stage) {
    if ($Only -contains "all") { return $true }
    return $Only -contains $stage
}

function NowIso { (Get-Date).ToString("o") }

function Get-SystemLoad {
    # 与 bench_servers.py 同口径：GetSystemTimes 差分（idle / kernel / user）
    $sig = @'
using System;
using System.Runtime.InteropServices;
public static class SysTimes {
  [StructLayout(LayoutKind.Sequential)] public struct FILETIME { public uint lo; public uint hi; }
  [DllImport("kernel32.dll")] public static extern bool GetSystemTimes(out FILETIME idle, out FILETIME kernel, out FILETIME user);
  public static double[] Read() {
    FILETIME i, k, u;
    if (!GetSystemTimes(out i, out k, out u)) return new double[] { -1, -1, -1 };
    return new double[] { ((long)i.hi << 32) | i.lo, ((long)k.hi << 32) | k.lo, ((long)u.hi << 32) | u.lo };
  }
}
'@
    if (-not ("SysTimes" -as [type])) { Add-Type -TypeDefinition $sig | Out-Null }
    return [SysTimes]::Read()
}

function Get-LoadFraction($before, $after) {
    if ($null -eq $before -or $null -eq $after) { return $null }
    $di = $after[0] - $before[0]
    $dt = ($after[1] - $before[1]) + ($after[2] - $before[2])
    if ($dt -le 0) { return $null }
    return [math]::Round(1.0 - ($di / $dt), 3)
}

function Get-ExeIdentity([string]$path) {
    if (-not (Test-Path $path)) { return $null }
    $item = Get-Item $path
    return [ordered]@{
        path   = $item.FullName
        bytes  = $item.Length
        sha256 = (Get-FileHash $path -Algorithm SHA256).Hash
        mtime  = $item.LastWriteTime.ToString("o")
    }
}

# ---- 0. 环境与二进制标识 ----------------------------------------------------
# 铁律 9：测量一律针对**冻结副本**。owner 们在并行重编 build/bin，直接用它会
# 让同一个 tag 的不同阶段跑在两个不同二进制上（实测已踩过一次）。
$exeSrc = if ($Exe) { (Resolve-Path $Exe).Path } else { Join-Path $repo "build\bin\fpgalsp-native.exe" }
$freezeDir = Join-Path $env:TEMP "fpgalsp-gate\exe"
New-Item -ItemType Directory -Force -Path $freezeDir | Out-Null
$freeze = Join-Path $freezeDir "$Tag.exe"
Copy-Item -LiteralPath $exeSrc -Destination $freeze -Force
$exePath = $freeze
$env:FPGALSP_NATIVE_EXE = $freeze

$gitHead = (& git -C $repo rev-parse HEAD).Trim()
$gitDirty = ((& git -C $repo status --porcelain) | Measure-Object).Count

$meta = [ordered]@{
    tag            = $Tag
    started_at     = NowIso
    machine        = "$env:COMPUTERNAME ($([Environment]::ProcessorCount) logical cores)"
    git_head       = $gitHead
    git_dirty_files = $gitDirty
    exe_source     = Get-ExeIdentity $exeSrc
    exe            = Get-ExeIdentity $freeze
    exe_frozen_path = $freeze
    exe_selected_by = if ($Exe) { "FPGALSP_NATIVE_EXE→冻结副本" } else { "build/bin→冻结副本" }
    rounds         = $Rounds
    samples        = $Samples
    sizes          = $Sizes
    stages         = $Only
    protocol_runs  = $ProtocolRuns
    # coverage：本 tag 到底覆盖了什么。fast tag（latency-only）**不能**被当成"协议无差异"
    # 的证据 —— 它根本没跑协议探针。VERIFY.md 里每个快 tag 都要带这个字段。
    coverage       = ($Only -join "+")
    covers_latency = (Want "latency")
    covers_reports = (Want "reports")
    covers_phase2  = (Want "phase2")
    covers_protocol = (Want "protocol")
    covers_index   = (Want "index")
    python         = (& python --version 2>&1)
}
Write-Host "[gate] tag=$Tag exe=$(if ($meta.exe) { "$($meta.exe.sha256.Substring(0,12)) $($meta.exe.bytes)B @$($meta.exe.mtime)" } else { 'MISSING' })" -ForegroundColor Cyan
Write-Host "[gate] git=$($gitHead.Substring(0,8)) dirty=$gitDirty rounds=$Rounds samples=$Samples"

$loadBefore = Get-SystemLoad
$stageErrors = [ordered]@{}

# ---- 1. 端到端延迟（9 方法 + 队头阻塞 + cancel + 连打）---------------------
if (Want "latency") {
    Write-Host "[gate] latency driver ..." -ForegroundColor Yellow
    $out = Join-Path $tagDir "latency.json"
    try {
        & python "tools\perf\latency\latency_driver.py" `
            --sizes ($Sizes -join ",") --rounds $Rounds --samples $Samples `
            --fast 10 --report-mb $ReportMb --out $out 2>&1 | Select-Object -Last 2
        if ($LASTEXITCODE -ne 0) { throw "latency_driver exit=$LASTEXITCODE" }
    } catch { $stageErrors["latency"] = "$_" }
}

# ---- 2. phase2（warm / edit-loop / 10MB 报告 / 进程内存）--------------------
if (Want "phase2") {
    Write-Host "[gate] bench_servers phase2 ..." -ForegroundColor Yellow
    $out = Join-Path $tagDir "phase2.json"
    try {
        & python "tools\audit\bench_servers.py" --only phase2 `
            --phase2-sizes ($Sizes -join ",") --phase2-iterations $Samples `
            --phase2-edits 5 --phase2-fast 10 --phase2-report-mb $ReportMb `
            --phase2-repeats 5 --out $out 2>&1 | Select-Object -Last 2
        if ($LASTEXITCODE -ne 0) { throw "bench_servers exit=$LASTEXITCODE" }
    } catch { $stageErrors["phase2"] = "$_" }
}

# ---- 2b. 报告路径窄口径（F3 的主战场，约 2 分钟）--------------------------
if (Want "reports") {
    Write-Host "[gate] reports probe (10 MB timing + area) ..." -ForegroundColor Yellow
    $out = Join-Path $tagDir "reports.json"
    try {
        & python "tools\perf\gate\reports_probe.py" --report-mb $ReportMb --repeats 5 --out $out 2>&1 |
            Select-Object -Last 2
        if ($LASTEXITCODE -ne 0) { throw "reports_probe exit=$LASTEXITCODE" }
    } catch { $stageErrors["reports"] = "$_" }
}

# ---- 3. 索引 stamps 场景（F2 的主指标）-------------------------------------
if (Want "index") {
    Write-Host "[gate] perf_index (stamps + incremental) ..." -ForegroundColor Yellow
    $out = Join-Path $tagDir "perf_index.txt"
    try {
        $exeIndex = Join-Path $repo "build\bin\perf_index.exe"
        if (-not (Test-Path $exeIndex)) { throw "perf_index.exe 不存在（需要 owner 先构建 perf_index）" }
        & $exeIndex --scenario all --iters $Samples --warmup 3 *> $out
        if ($LASTEXITCODE -ne 0) { throw "perf_index exit=$LASTEXITCODE" }
    } catch { $stageErrors["index"] = "$_" }
}

# ---- 4. 协议输出快照（与 baseline 逐字节对比）------------------------------
if ((Want "protocol") -and (-not $NoProtocol)) {
    Write-Host "[gate] protocol snapshot (LSP + MCP, ADC/SDC x$ProtocolRuns) ..." -ForegroundColor Yellow
    # 第一遍跑全量（matrix/commands/boundary），第 2、3 遍只跑 boundary 用于 ADC/SDC 复现检查
    for ($run = 1; $run -le [Math]::Max(1, $ProtocolRuns); $run++) {
        $suffix = if ($run -eq 1) { "" } else { "-run$run" }
        $only = if ($run -eq 1) { @("--only", "all") } else { @("--only", "boundary") }
        $out = Join-Path $tagDir "lsp-raw$suffix.json"
        try {
            # 钉死场景：基线是 --project %WORKSPACE%/CorpusAlpha --only all（见 baseline/lsp-raw.json.meta.argv），
            # 换工程/换 only 都会让 payload 天然不同 —— 那不是回归。
            $probeArgs = @("tools\audit\probe_lsp.py", "--native-exe", $exePath,
                           "--project", "%WORKSPACE%/CorpusAlpha", "--out", $out) + $only
            & python @probeArgs 2>&1 | Select-Object -Last 1
            if ($LASTEXITCODE -ne 0) { throw "lsp probe run$run exit=$LASTEXITCODE" }
        } catch { $stageErrors["protocol-lsp$suffix"] = "$_" }
    }
    # MCP 一遍足够（ADC/SDC 是 LSP 侧问题）
    $out = Join-Path $tagDir "mcp-raw.json"
    try {
        & python "tools\audit\probe_mcp.py" --exe $exePath --copy-projects --out $out 2>&1 | Select-Object -Last 2
        if ($LASTEXITCODE -ne 0) { throw "mcp probe exit=$LASTEXITCODE" }
    } catch { $stageErrors["protocol-mcp"] = "$_" }
}

# ---- 4b. 单元测试（可选；**先重编目标再跑**）-------------------------------
# Lead 踩过的坑：`unit_index_service.exe` 还是加 TTL 之前编的旧二进制，于是
# "没检测到磁盘变化" —— 测的是上一个版本。所以这里强制先 build 再 ctest。
if (Want "unit") {
    $targets = if ($UnitTarget) { @($UnitTarget) } else { @() }
    if (-not $targets) { throw "-Only unit 需要同时给 -UnitTarget（如 unit_index_service）" }
    Write-Host "[gate] unit test (rebuild first): $($targets -join ', ')" -ForegroundColor Yellow
    try {
        & pwsh -File (Join-Path $repo "tools\perf\build.ps1") -Target $targets -Jobs 12 2>&1 |
            Select-Object -Last 3
        if ($LASTEXITCODE -ne 0) { throw "build $($targets -join ',') exit=$LASTEXITCODE" }
        # ctest -R 用点号转义：unit_index_service -> unit\.index_service
        $pattern = ($targets | ForEach-Object { ($_ -replace '^unit_', 'unit\.') }) -join "|"
        & ctest --test-dir (Join-Path $repo "build") -R $pattern --output-on-failure 2>&1 |
            Tee-Object -FilePath (Join-Path $tagDir "ctest.txt") | Select-Object -Last 6
        $meta_unit = @{ targets = $targets; pattern = $pattern; exit = $LASTEXITCODE }
        if ($LASTEXITCODE -ne 0) { throw "ctest $pattern exit=$LASTEXITCODE" }
    } catch { $stageErrors["unit"] = "$_" }
}

# ---- 5. 插件侧（F6 专用，默认跳过）----------------------------------------
if ($WithVscode) {
    Write-Host "[gate] vscode benches ..." -ForegroundColor Yellow
    foreach ($bench in @("bench_keystroke.js", "bench_events.js")) {
        $out = Join-Path $tagDir ("vscode-" + [IO.Path]::GetFileNameWithoutExtension($bench) + ".json")
        try {
            & node "tools\perf\vscode\$bench" --json $out 2>&1 | Select-Object -Last 2
            if ($LASTEXITCODE -ne 0) { throw "$bench exit=$LASTEXITCODE" }
        } catch { $stageErrors["vscode-$bench"] = "$_" }
    }
}

$loadAfter = Get-SystemLoad
$meta.finished_at = NowIso
$meta.cpu_busy_fraction = Get-LoadFraction $loadBefore $loadAfter
$meta.stage_errors = $stageErrors

# 铁律 9 的后半段：冻结副本让我们全程用同一二进制；再回看**源文件**有没有在测量期间变过。
# 变过 → 本 tag 数据作废（不是因为数字错了，而是因为它对应的是一个"不存在的瞬间构建"）。
$srcEnd = Get-ExeIdentity $exeSrc
$meta.exe_source_at_end = $srcEnd
$meta.source_changed_during_run = ($null -eq $meta.exe_source -or $null -eq $srcEnd -or
                                   $meta.exe_source.sha256 -ne $srcEnd.sha256)
if ($SimulateExeChange) {
    $meta.source_changed_during_run = $true
    $meta.simulated_exe_change = $true
}
$meta.valid = -not $meta.source_changed_during_run
if (-not $meta.valid) {
    $meta.invalid_reason = "测量期间源 exe 被重编（$($meta.exe_source.sha256.Substring(0,12)) → $(if ($srcEnd) { $srcEnd.sha256.Substring(0,12) } else { 'missing' })）：本 tag 作废，请重跑"
    Write-Host "[gate] ⚠ $($meta.invalid_reason)" -ForegroundColor Red
} else {
    Write-Host "[gate] exe 全程稳定：$($meta.exe_source.sha256.Substring(0,12))（冻结副本 $($meta.exe_frozen_path)）" -ForegroundColor Green
}

Write-Host "[gate] analyzing (load=$($meta.cpu_busy_fraction)) ..." -ForegroundColor Yellow

# ---- 6. 汇总 / 对比 baseline ----------------------------------------------
$summary = Join-Path $resultsDir "$Tag.json"
$analyzeArgs = @(
    "tools\perf\gate\verify.py",
    "--tag", $Tag,
    "--tag-dir", $tagDir,
    "--baseline-dir", $baselineDir,
    "--meta", (Join-Path $tagDir "meta.json"),
    "--out", $summary
)
if ($NormalizeQorSuggestions) { $analyzeArgs += "--normalize-qor-suggestions" }
$meta | ConvertTo-Json -Depth 6 | Out-File -Encoding utf8 (Join-Path $tagDir "meta.json")
& python @analyzeArgs
$code = $LASTEXITCODE
if (-not $meta.valid) { $code = 4 }   # 4 = tag 作废（源 exe 在测量期间变过）

Write-Host ""
Write-Host "[gate] 结果: $summary" -ForegroundColor Green
if (Test-Path "$summary.md") { Get-Content "$summary.md" }
exit $code