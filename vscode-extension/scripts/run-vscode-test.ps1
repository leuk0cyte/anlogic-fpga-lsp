# Run the real-VS-Code extension-host test suite (opens a window, ~10-30 s).
#
#   powershell -ExecutionPolicy Bypass -File scripts\run-vscode-test.ps1 [-Project C:/work/CorpusAlpha]
#
# Verifies the actual editor path: language client connects, documentSymbol / completion /
# definition round-trip through LSP, an in-memory ADC edit produces a diagnostic, and the
# reports command runs. Exits non-zero on failure.

param(
    [string]$Project = "C:/work/CorpusAlpha",
    [string]$CodeCli = "C:\Program Files\Microsoft VS Code\bin\code.cmd"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path $root ".vsc_test"
$suite = Join-Path $root "test\vscode-suite"
$result = Join-Path $suite "result.txt"

if (-not (Test-Path $CodeCli)) {
    Write-Error "VS Code CLI not found: $CodeCli (pass -CodeCli)"
}
if (-not (Test-Path $Project)) {
    Write-Error "project folder not found: $Project"
}

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $work "ext"), (Join-Path $work "data") | Out-Null
Remove-Item $result -ErrorAction SilentlyContinue

& $CodeCli --new-window --wait --disable-gpu `
    --extensions-dir (Join-Path $work "ext") `
    --user-data-dir (Join-Path $work "data") `
    --extensionDevelopmentPath $root `
    --extensionTestsPath $suite `
    $Project | Out-Null

if (-not (Test-Path $result)) {
    Write-Error "VS Code test host produced no result.txt (window failed to start tests)"
}
Get-Content $result
if (Select-String -Path $result -Pattern "SUITE: FAIL" -Quiet) { exit 1 }
exit 0
