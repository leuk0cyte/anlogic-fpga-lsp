# =============================================================================
#  tools/perf/build.ps1 —— 共享 build 目录的串行构建闸门（FIFO 排队版）
# =============================================================================
#  为什么需要：本仓库多个 agent 共用 `build/` 与同一份 ninja 图。ninja 在同一
#  构建目录里并发跑两个进程会互相踩。这里把构建串行化。
#
#  为什么是 FIFO 而不是"抢锁"：v1 用 CreateNew 轮询抢锁，8 个 agent 同时重试时
#  会出现**饥饿**（有人排 30 分钟一次没抢到，每次都输给下一个）。
#  v2 改成**票据队列**：每个调用先领一张按时间的票据，只有票据最早的人可以建锁。
#  先来先服务，不存在"运气不好一直抢不到"。
#
#  用法：
#     pwsh -File tools\perf\build.ps1 -Target perf_index
#     pwsh -File tools\perf\build.ps1 -Target perf_index,unit_hdl_index -Jobs 8
#     pwsh -File tools\perf\build.ps1 -Reconfigure        # 重新 configure（新增 probe 后需要）
#     pwsh -File tools\perf\build.ps1 -Target perf_td -QueueTimeoutMinutes 60
#
#  退出码：0 成功；非 0 失败（打印 ninja 尾部输出）；2 = 等锁超时。
# =============================================================================
param(
    [string[]]$Target = @(),
    [int]$Jobs = 12,
    [switch]$Reconfigure,
    [string]$BuildDir = "build",
    [int]$QueueTimeoutMinutes = 45,
    [int]$LockTimeoutMinutes = 30
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildPath = Join-Path $repo $BuildDir
if (-not (Test-Path $buildPath)) { throw "build 目录不存在: $buildPath" }

$lockPath   = Join-Path $buildPath '.perf-build.lock'
$queueDir   = Join-Path $buildPath '.perf-build.queue'
if (-not (Test-Path $queueDir)) { New-Item -ItemType Directory -Force $queueDir | Out-Null }

# ---- 前置检查：build.ninja 里必须有 /O2 -------------------------------------
# 2026-11-19 08:32–08:49 真的发生过一次：cmake 被 configure 成
# CMAKE_CXX_FLAGS_RELEASE=""（无 /O2 /DNDEBUG /EHsc），那段时间重建出来的
# 一切（包括性能探针）都会慢 1.9–5.9×，测出来的数全是假的。
$ninjaFile = Join-Path $buildPath 'build.ninja'
if (Test-Path $ninjaFile) {
    $o2Count = (Select-String -Path $ninjaFile -Pattern '/O2' -AllMatches | Measure-Object).Count
    if ($o2Count -lt 1) {
        throw @"
build.ninja 里找不到 /O2 —— 这个构建目录的 Release 优化被清掉了。
  历史原因：CMAKE_CXX_FLAGS_RELEASE 曾被 configure 成空串。
  修法：pwsh -File tools\perf\build.ps1 -Reconfigure
"@
    }
    Write-Host "[build] 前置检查通过：build.ninja 含 $o2Count 处 /O2"
}

# ---- 领票 ------------------------------------------------------------------
# 票据名固定宽度时间戳 + pid + 随机后缀，按名字升序即到达顺序。
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddHHmmssfff')
$ticketPath = Join-Path $queueDir ("{0}-{1}-{2}.t" -f $stamp, $PID, (Get-Random -Maximum 9999))
Set-Content -Path $ticketPath -Value "pid=$PID target=$($Target -join ',')" -Encoding ascii
$myTicket = Split-Path -Leaf $ticketPath
Write-Host "[build] 领票 $myTicket（队列中还排着 $((Get-ChildItem $queueDir -Filter *.t | Measure-Object).Count - 1) 个）"

$deadline = (Get-Date).AddMinutes($QueueTimeoutMinutes)
$held = $false
try {
    while (-not $held) {
        if ((Get-Date) -gt $deadline) {
            Write-Host "[build] 等锁超时（$QueueTimeoutMinutes 分钟）"
            exit 2
        }

        if (Test-Path $lockPath) {
            $age = (Get-Date) - (Get-Item $lockPath).LastWriteTime
            if ($age.TotalMinutes -gt $LockTimeoutMinutes) {
                Write-Host "[build] 清理陈旧锁（$([int]$age.TotalMinutes) 分钟）"
                Remove-Item $lockPath -Force -ErrorAction SilentlyContinue
            } else {
                Start-Sleep -Milliseconds 1500
                continue
            }
        }

        # 队首才能建锁
        $head = Get-ChildItem $queueDir -Filter *.t | Sort-Object Name | Select-Object -First 1
        if (-not $head -or $head.Name -ne $myTicket) {
            Start-Sleep -Milliseconds 800
            continue
        }

        try {
            $fs = [System.IO.File]::Open($lockPath, 'CreateNew', 'Write', 'None')
            $fs.Close()
            $held = $true
        } catch {
            Start-Sleep -Milliseconds 500
        }
    }

    Remove-Item $ticketPath -Force -ErrorAction SilentlyContinue
    Write-Host "[build] 拿到锁，开始构建"

    $cmake = (Get-Command cmake).Source
    if ($Reconfigure) {
        Write-Host "[build] configure $BuildDir"
        & $cmake --preset windows-msvc-release | Out-Host
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    if ($Target.Count -gt 0) {
        foreach ($t in $Target) {
            Write-Host "[build] target=$t jobs=$Jobs"
            & $cmake --build $BuildDir --target $t -- -j $Jobs
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
    }
} finally {
    Remove-Item $ticketPath -Force -ErrorAction SilentlyContinue
    if ($held) { Remove-Item $lockPath -Force -ErrorAction SilentlyContinue }
}
exit 0