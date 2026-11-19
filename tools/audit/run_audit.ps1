<#
.SYNOPSIS
    One-command static-analysis audit for fpga-lsp-cpp.

.DESCRIPTION
    Reproduces the whole audit pipeline end to end:

      0. precondition  build/compile_commands.json exists and covers every
                       src/*.cpp (a stale database silently analyses stale code)
      1. db rewrite    tools/audit/make_clang_db.py  cl.exe argv -> clang argv
                       (clang's GNU driver ignores /std:c++23 and would fall back
                        to C++14 and then fail on every C++17/20/23 header)
      2. clang-tidy    tools/audit/run_clang_tidy.py  one process per TU
      3. MSVC /analyze tools/audit/run_msvc_analyze.ps1  C6xxx / C26xxx
      4. summary       tools/audit/summarize.py  deduplicated counts, per-module
                       density, function-length distribution

    Steps 2 and 3 both need the MSVC environment (the clang GNU driver resolves
    the STL through %INCLUDE% set by vcvars64.bat).  If INCLUDE is not already
    set, this script re-launches itself through tools/audit/with_vcvars.cmd.

.PARAMETER AuditDir
    Output directory for reports, relative to the repo root.
    Default: audit/2026-11-19/static

.PARAMETER Jobs
    Parallel clang-tidy processes.  Default 16.  Each TU of this project peaks
    at roughly 1 GB RSS (abseil + Verible headers), so do not simply pass the
    core count.

.PARAMETER SkipMsvcAnalyze
    Skip the MSVC /analyze pass (it takes ~2 min and needs cl.exe).

.PARAMETER SkipTidy
    Reuse existing per-TU logs and only re-aggregate + summarise.

.EXAMPLE
    pwsh -File tools\audit\run_audit.ps1 -Jobs 16
#>
[CmdletBinding()]
param(
    [string]$AuditDir = 'audit\2026-11-19\static',
    [int]$Jobs = 16,
    [int]$TimeoutSec = 900,
    [switch]$SkipMsvcAnalyze,
    [switch]$SkipTidy
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root

# ---------------------------------------------------------------- 0. vcvars
# The clang GNU driver finds the MSVC STL through the INCLUDE environment
# variable that vcvars64.bat sets.  Without it every TU fails on <string>.
if (-not $env:INCLUDE -or -not $env:VCToolsInstallDir) {
    Write-Host '[audit] MSVC environment not detected -- re-launching under vcvars64.bat'
    $self = $PSCommandPath
    $extra = @()
    if ($SkipMsvcAnalyze) { $extra += '-SkipMsvcAnalyze' }
    if ($SkipTidy) { $extra += '-SkipTidy' }
    $argLine = '-NoProfile -File "{0}" -AuditDir "{1}" -Jobs {2} -TimeoutSec {3} {4}' -f `
        $self, $AuditDir, $Jobs, $TimeoutSec, ($extra -join ' ')
    cmd /c "tools\audit\with_vcvars.cmd pwsh $argLine"
    exit $LASTEXITCODE
}

$auditPath = Join-Path $root $AuditDir
New-Item -ItemType Directory -Force -Path $auditPath | Out-Null

# ------------------------------------------------------ 0b. db precondition
$db = Join-Path $root 'build\compile_commands.json'
if (-not (Test-Path $db)) {
    Write-Error @"
build\compile_commands.json 不存在。请先配置构建：
    cmake --preset windows-msvc-release
（preset 已设置 CMAKE_EXPORT_COMPILE_COMMANDS=ON）
"@
}
$dbSrc = @((Get-Content $db -Raw | ConvertFrom-Json) |
    Where-Object { $_.file -replace '/', '\' -match '\\src\\' }).Count
$diskSrc = @(Get-ChildItem (Join-Path $root 'src') -Recurse -Filter *.cpp).Count
Write-Host "[audit] src TUs: db=$dbSrc  on disk=$diskSrc"
if ($dbSrc -ne $diskSrc) {
    Write-Warning "编译数据库 ($dbSrc) 与磁盘上的 src/*.cpp ($diskSrc) 数量不一致 —— 数据库已过期，请重新 configure 后再审计，否则会漏分析新文件。"
}

# ------------------------------------------------------------- 1. rewrite db
Write-Host '[audit] 1/4  make_clang_db.py'
python tools\audit\make_clang_db.py
if ($LASTEXITCODE -ne 0) { throw 'make_clang_db.py failed' }

# ------------------------------------------------------------ 2. clang-tidy
if (-not $SkipTidy) {
    Write-Host "[audit] 2/4  clang-tidy ($Jobs jobs, ${TimeoutSec}s/TU)"
    $env:FPGA_LSP_AUDIT_DIR = $AuditDir
    python tools\audit\run_clang_tidy.py --jobs $Jobs --timeout $TimeoutSec
    if ($LASTEXITCODE -ne 0) { throw 'run_clang_tidy.py failed' }
} else {
    Write-Host '[audit] 2/4  clang-tidy SKIPPED (reusing existing logs)'
    $env:FPGA_LSP_AUDIT_DIR = $AuditDir
    python tools\audit\run_clang_tidy.py --aggregate-only
}

# ----------------------------------------------------- 3. MSVC /analyze
if (-not $SkipMsvcAnalyze) {
    Write-Host '[audit] 3/4  MSVC /analyze'
    & pwsh -NoProfile -File tools\audit\run_msvc_analyze.ps1 -Jobs 8 -TimeoutSec 1200 `
        -OutFile (Join-Path $AuditDir 'msvc-analyze.txt')
    if ($LASTEXITCODE -ne 0) { Write-Warning 'msvc /analyze failed (non-fatal)' }
} else {
    Write-Host '[audit] 3/4  MSVC /analyze SKIPPED'
}

# ------------------------------------------------------------- 4. summary
Write-Host '[audit] 4/4  summarize.py'
python tools\audit\summarize.py --audit-dir $AuditDir
if ($LASTEXITCODE -ne 0) { throw 'summarize.py failed' }

Write-Host ''
Write-Host '================ 审计完成 ================'
Write-Host "audit dir : $AuditDir"
Write-Host '  clang-tidy.txt / .json   原始与结构化诊断'
Write-Host '  msvc-analyze.txt         MSVC /analyze（含 analyzer control 自检段）'
Write-Host '  summary.md / .json       去重后口径的统计与函数长度分布'
Write-Host '  PERFORMANCE.md / SECURITY.md / MODERNIZATION.md   三份维度报告'
Write-Host '  ../REPORT.md             汇总报告'
Write-Host '=========================================='