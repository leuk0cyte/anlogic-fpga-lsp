# =============================================================================
#  tools/perf/devshell.ps1 —— 把 MSVC 环境导进当前 PowerShell 进程
# =============================================================================
#  用法（必须在同一进程内 dot-source）：
#      . .\tools\perf\devshell.ps1
#      cmake --preset windows-msvc-release
#      cmake --build build --target perf_index -- -j 12
#
#  为什么需要：本机 cl.exe 不在 PATH 上，cmake preset 里写的是裸 `cl`，
#  所以必须先跑 vcvars64.bat 把环境灌进当前进程。
#
#  ⚠ 历史坑（2026-11-19，perf-build 报告）：
#     早期实现直接在脚本作用域里用 `$name` 存环境变量名，dot-source 会把
#     `$name` 泄漏到**调用者**的作用域（PowerShell 变量大小写不敏感），
#     把调用者的 `-Name` 参数覆盖成 `__VSCMD_PREINIT_PATH`。
#     现在把所有临时变量关进函数内部，dot-source 只留下环境变量本身。
# =============================================================================
$ErrorActionPreference = 'Stop'

function Import-MsvcEnvironment {
    $vcvarsCandidates = @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    )
    $vcvarsPath = $vcvarsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $vcvarsPath) {
        throw "找不到 vcvars64.bat（找过: $($vcvarsCandidates -join ', ')）"
    }

    # `set` 输出 vcvars 之后的完整进程环境；逐行灌回当前进程。
    $envLines = cmd /c "`"$vcvarsPath`" >nul 2>&1 && set"
    foreach ($envLine in $envLines) {
        if ($envLine -match '^([^=]+)=(.*)$') {
            $envKey = $matches[1]
            if ($envKey -match '^=') { continue }   # "=C:=C:\..." 盘符 cwd 变量，跳过
            [Environment]::SetEnvironmentVariable($envKey, $matches[2], 'Process')
        }
    }

    $clCommand = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $clCommand) { throw "vcvars64.bat 已执行但 PATH 里仍找不到 cl.exe" }
    Write-Host "[devshell] cl.exe = $($clCommand.Source)"
    Write-Host "[devshell] VCToolsVersion = $env:VCToolsVersion"
}

Import-MsvcEnvironment
# 函数本身留在作用域里不碍事；临时变量全部是函数局部，不会泄漏给调用者。