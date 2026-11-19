# Re-run every VS Code plugin-side measurement and regenerate tools/perf/vscode/results/*.json.
#
#   pwsh -File tools/perf/vscode/run_all.ps1
#
# Node is enough for everything (no VS Code, no npm install); the LSP round-trip half uses the
# production exe read-only and a %TEMP% project copy.

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

Push-Location $here
try {
    Write-Host '== 0/6 prep: %TEMP% project copy' -ForegroundColor Cyan
    node prep_workspace.js

    Write-Host '== 1/6 generate synthetic report payloads (~26 MB)' -ForegroundColor Cyan
    node gen_payloads.js | Select-String -Pattern 'csv,'

    Write-Host '== 2/6 reports payload: parse + render' -ForegroundColor Cyan
    node --expose-gc bench_reports.js | Select-String -Pattern 'csv,|machine busy'

    Write-Host '== 3/6 event paths: providers/panels' -ForegroundColor Cyan
    node --expose-gc bench_events.js | Select-String -Pattern 'csv,|machine busy'

    Write-Host '== 4/6 activate + wiring (save/indexChanged/open)' -ForegroundColor Cyan
    node --expose-gc bench_activate.js | Select-String -Pattern 'csv,|machine busy'

    Write-Host '== 5/6 per-keystroke paths' -ForegroundColor Cyan
    node --expose-gc bench_keystroke.js | Select-String -Pattern 'csv,|machine busy'
    node --expose-gc bench_keystroke.js --kb 1024 | Select-String -Pattern 'csv,|machine busy'

    Write-Host '== 6/6 static audit + real LSP round-trip' -ForegroundColor Cyan
    node audit_static.js | Select-String -Pattern 'csv,'
    python bench_lsp_roundtrip.py

    Write-Host '== results' -ForegroundColor Green
    Get-ChildItem (Join-Path $here 'results') -File | Select-Object Name, Length | Format-Table -AutoSize
} finally {
    Pop-Location
}