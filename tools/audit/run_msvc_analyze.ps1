<#
.SYNOPSIS
    Run MSVC /analyze over src/ and summarize C26xxx / C6xxx diagnostics.

.DESCRIPTION
    Reuses the CMake compilation database (build/compile_commands.json) -- the same
    source of truth the clang pipeline is derived from -- so the analyzer sees the
    product flags (/permissive-, /std:c++23, /MT, defines), not an approximation.

    Phases:
      0. control   compile a scratch TU with two deliberate defects in %TEMP% with
                   the exact same switches.  If it does not report C6011/C6001 the
                   rest of the run is meaningless, so the result is printed in the
                   report header.  (This is how "0 findings" is proven to be a clean
                   result rather than a silently disabled analyzer.)
      1. rewrite   every src/ TU's cl command line into a response file under
                   %TEMP%\fpgalsp-audit-msvc, rewriting /Fo and /Fd into that
                   directory so build/ is never touched, forcing /W4 and /analyze.
      2. compile   `cl.exe @rsp` per TU, N in parallel, per-TU timeout.
      3. aggregate `file(line,col): warning C26xx: text` -> per-code, per-file and
                   full listing into audit/2026-11-18/static/msvc-analyze.txt.

    Note on C26xxx: those come from the C++ Core Guidelines checker, which is a
    separate plugin (CppCoreCheck.dll) shipped with an optional VS component.  If
    the plugin matching the active MSVC toolset is absent, `/analyze` still runs
    the native rule set (C6xxx) -- the report states which of the two happened
    instead of pretending C26xxx were "clean".

.EXAMPLE
    cmd /c tools\audit\with_vcvars.cmd pwsh -NoProfile -File tools\audit\run_msvc_analyze.ps1 -Jobs 4
#>
[CmdletBinding()]
param(
    [string]$CompileDb = '',
    [string]$OutFile = '',
    [int]$Jobs = 4,
    [int]$TimeoutSec = 900,
    [string]$CoreCheckPlugin = '',   # optional path to CppCoreCheck.dll (enables C26xxx)
    [switch]$NoRun,                  # only build the response files, do not compile
    [switch]$AggregateOnly
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $CompileDb) { $CompileDb = Join-Path $root 'build\compile_commands.json' }
if (-not $OutFile)   { $OutFile   = Join-Path $root 'audit\2026-11-18\static\msvc-analyze.txt' }
$tempDir = Join-Path $env:TEMP 'fpgalsp-audit-msvc'
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutFile) | Out-Null

Write-Host "[msvc] db  = $CompileDb"
Write-Host "[msvc] tmp = $tempDir"
Write-Host "[msvc] out = $OutFile"

function Split-Cmd([string]$line) {
    # MSVC command lines quote paths with spaces and nothing else.
    $m = [regex]::Matches($line, '"([^"]*)"|(\S+)')
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($x in $m) {
        if ($x.Groups[1].Success) { $out.Add($x.Groups[1].Value) } else { $out.Add($x.Groups[2].Value) }
    }
    return $out
}

# ------------------------------------------------------------ phase 1: rsp files
$entries = Get-Content -Raw -Path $CompileDb | ConvertFrom-Json
$srcPrefix = (Join-Path $root 'src') + '\'
$tuList = New-Object System.Collections.Generic.List[object]
$clExe = $null
$idx = 0
foreach ($e in $entries) {
    $file = [string]$e.file
    if (-not $file.Replace('/', '\').StartsWith($srcPrefix, [StringComparison]::OrdinalIgnoreCase)) { continue }
    $tokens = Split-Cmd ([string]$e.command)
    if (-not $clExe) { $clExe = $tokens[0] }
    $idx++
    $stem = ('tu{0:d3}' -f $idx)
    $obj = Join-Path $tempDir ("$stem.obj")
    $pdb = Join-Path $tempDir ("$stem.pdb")
    # NOTE: not $args -- that is a PowerShell automatic variable.
    $newArgs = New-Object System.Collections.Generic.List[string]
    foreach ($t in $tokens[1..($tokens.Count - 1)]) {
        if ($t -match '^[/-]Fo')      { $newArgs.Add('/Fo"' + $obj + '"'); continue }
        if ($t -match '^[/-]Fd')      { $newArgs.Add('/Fd"' + $pdb + '"'); continue }
        if ($t -match '^[/-]analyze') { continue }          # we add our own below
        if ($t -match '^[/-]W[0-4]$') { continue }          # force /W4
        $newArgs.Add($t)
    }
    $newArgs.Add('/W4')
    $newArgs.Add('/analyze')
    $newArgs.Add('/analyze:external-')
    if ($CoreCheckPlugin) { $newArgs.Add('/analyze:plugin'); $newArgs.Add($CoreCheckPlugin) }
    $rsp = Join-Path $tempDir ("$stem.rsp")
    Set-Content -Path $rsp -Value ($newArgs -join "`r`n") -Encoding ascii
    $tuList.Add([pscustomobject]@{
            Tu  = $file.Replace($root + '\', '').Replace('\', '/')
            Rsp = $rsp
            Log = Join-Path $tempDir ("$stem.log")
            Cwd = [string]$e.directory
        })
}
Write-Host "[msvc] $($tuList.Count) src TUs, compiler = $clExe"
if ($tuList.Count -eq 0) { throw "no src TUs found in $CompileDb" }

# ------------------------------------------------------- phase 0: control test
$controlText = ''
if (-not $AggregateOnly) {
    $ctl = Join-Path $tempDir 'selfcheck.cpp'
    @'
// Positive control for the /analyze pipeline.  Two deliberate defects; never
// part of the product build.
int Deref(int flag) {
    int* p = nullptr;
    if (flag) { return *p; }   // C6011: null pointer dereference
    int uninit;
    return uninit;             // C6001: uninitialized memory read
}
'@ | Set-Content -Path $ctl -Encoding ascii
    $ctlOut = ''
    try {
        $ctlOut = (& $clExe /nologo /c /W4 /analyze /analyze:external- /std:c++23 `
                "/Fo$tempDir\selfcheck.obj" $ctl 2>&1 | Out-String)
    } catch { $ctlOut = "control failed to run: $_" }
    $ctlHit = ($ctlOut -match 'C6011') -and ($ctlOut -match 'C6001')
    $controlText = @(
        "control TU    : $ctl",
        "control result: " + $(if ($ctlHit) { 'OK -- analyzer reported C6011 + C6001 (the pipeline is live, so a 0-finding run below is a real result)' } else { 'FAILED -- analyzer produced no diagnostics; treat the whole run as INVALID' }),
        "control output:",
        (($ctlOut -split "`r?`n") | Where-Object { $_.Trim() } | ForEach-Object { '    ' + $_ }) -join "`r`n"
    ) -join "`r`n"
    Write-Host ("[msvc] control: " + $(if ($ctlHit) { 'analyzer live (C6011+C6001)' } else { 'ANALYZER INACTIVE' }))
}

# ------------------------------------------------------------ phase 2: compile
if (-not $AggregateOnly -and -not $NoRun) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $done = 0
    $tuList | ForEach-Object -Parallel {
        $item = $_
        $cli = $using:clExe
        $timeout = $using:TimeoutSec
        $p = Start-Process -FilePath $cli -ArgumentList ('@"' + $item.Rsp + '"') -WorkingDirectory $item.Cwd `
            -RedirectStandardOutput ($item.Log + '.out') -RedirectStandardError ($item.Log + '.err') `
            -PassThru -NoNewWindow
        $timedOut = $false
        try {
            Wait-Process -Id $p.Id -Timeout $timeout -ErrorAction Stop
        } catch {
            $timedOut = $true
            try { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } catch { }
        }
        $text = ''
        foreach ($f in @($item.Log + '.out', $item.Log + '.err')) {
            if (Test-Path $f) { $text += (Get-Content -Raw -Path $f -ErrorAction SilentlyContinue) }
        }
        if ($timedOut) { $text += "`r`n[timeout after $timeout s]`r`n" }
        Set-Content -Path $item.Log -Value $text -Encoding utf8
        Remove-Item -Force -ErrorAction SilentlyContinue ($item.Log + '.out'), ($item.Log + '.err')
        [pscustomobject]@{ Tu = $item.Tu; TimedOut = $timedOut; ExitCode = $(if ($timedOut) { -9 } else { $p.ExitCode }) }
    } -ThrottleLimit $Jobs | ForEach-Object {
        $done++
        Write-Host ("  [{0}/{1}] {2}" -f $done, $tuList.Count, $_.Tu)
    }
    $sw.Stop()
    Write-Host ("[msvc] compile phase took {0:n1} min" -f ($sw.Elapsed.TotalMinutes))
}

# ------------------------------------------------------------ phase 3: aggregate
$diagRe = [regex]'^(?<file>.+?)\((?<line>\d+)(?:,(?<col>\d+))?\)\s*:\s*(?<sev>warning|error|fatal error|info)\s+(?<code>[A-Z]+\d+)\s*:\s*(?<msg>.*)$'
$diags = New-Object System.Collections.Generic.List[object]
$tuStatus = New-Object System.Collections.Generic.List[object]
$rawLines = New-Object System.Collections.Generic.List[string]
foreach ($tu in $tuList) {
    if (-not (Test-Path $tu.Log)) { $tuStatus.Add([pscustomobject]@{ Tu = $tu.Tu; Status = 'not-run'; Codes = 0 }); continue }
    $text = Get-Content -Raw -Path $tu.Log
    $n = 0
    $rawLines.Add('')
    $rawLines.Add("### TU: $($tu.Tu)")
    foreach ($line in ($text -split "`r?`n")) {
        if (-not $line.Trim()) { continue }
        $rawLines.Add('    ' + $line)
        $m = $diagRe.Match($line)
        if ($m.Success) {
            $n++
            $diags.Add([pscustomobject]@{
                    File = $m.Groups['file'].Value.Replace($root + '\', '').Replace('\', '/')
                    Line = [int]$m.Groups['line'].Value
                    Col  = $(if ($m.Groups['col'].Success) { [int]$m.Groups['col'].Value } else { 0 })
                    Sev  = $m.Groups['sev'].Value
                    Code = $m.Groups['code'].Value
                    Msg  = $m.Groups['msg'].Value.Trim()
                })
        }
    }
    $status = if ($text -match '\[timeout after') { 'timeout' } elseif ($text -match 'fatal error|error C\d+') { 'has-errors' } else { 'ok' }
    $tuStatus.Add([pscustomobject]@{ Tu = $tu.Tu; Status = $status; Codes = $n })
}

$c26999 = $diags | Where-Object { $_.Code -match '^C26\d{3}$' }
$sw2 = New-Object System.Collections.Generic.List[string]
$sw2.Add('=' * 100)
$sw2.Add('fpga-lsp-cpp static analysis -- MSVC /analyze (C6xxx native rules, C26xxx when Core Check is installed)')
$sw2.Add('=' * 100)
$sw2.Add("generated   : $(Get-Date -Format o)")
$sw2.Add("compiler    : $clExe")
$sw2.Add("db          : $CompileDb")
$sw2.Add("scope       : src/ only, $($tuList.Count) TUs, /W4 /analyze /analyze:external- /std:c++23 (from the product DB)")
$sw2.Add("objects/pdb : $tempDir  (build/ is not written to)")
$sw2.Add("plugin      : " + $(if ($CoreCheckPlugin) { $CoreCheckPlugin } else { 'none (/analyze native rules only -- C26xxx Core Guidelines codes need CppCoreCheck.dll)' }))
$sw2.Add("diagnostics : $($diags.Count)  (C26xxx: $($c26999.Count))")
$sw2.Add('')
$sw2.Add('-' * 100)
$sw2.Add('0. ANALYZER CONTROL (proves the run is not silently a no-op)')
$sw2.Add('-' * 100)
if ($controlText) { $sw2.Add($controlText) } else { $sw2.Add('  (control not re-run in -AggregateOnly mode; see tu*.log for build output)') }
$sw2.Add('')
$sw2.Add('-' * 100)
$sw2.Add('1. DIAGNOSTICS BY CODE')
$sw2.Add('-' * 100)
if ($diags.Count -eq 0) { $sw2.Add('  (no C26xxx / C6xxx diagnostics in src/)') }
foreach ($g in ($diags | Group-Object Code | Sort-Object Count -Descending)) {
    $sw2.Add(("  {0,5}  {1,-10} {2}" -f $g.Count, $g.Name, ($g.Group[0].Msg)))
}
$sw2.Add('')
$sw2.Add('-' * 100)
$sw2.Add('2. DIAGNOSTICS BY FILE')
$sw2.Add('-' * 100)
foreach ($g in ($diags | Group-Object File | Sort-Object Count -Descending)) {
    $sw2.Add(("  {0,5}  {1}" -f $g.Count, $g.Name))
}
$sw2.Add('')
$sw2.Add('-' * 100)
$sw2.Add('3. ALL DIAGNOSTICS (file:line)')
$sw2.Add('-' * 100)
foreach ($d in ($diags | Sort-Object File, Line)) {
    $sw2.Add(("  {0}:{1}:{2}  {3}  {4}: {5}" -f $d.File, $d.Line, $d.Col, $d.Sev, $d.Code, $d.Msg))
}
$sw2.Add('')
$sw2.Add('-' * 100)
$sw2.Add('4. COMPILE ERRORS / TIMEOUTS (TU not analysed)')
$sw2.Add('-' * 100)
$bad = $tuStatus | Where-Object { $_.Status -ne 'ok' }
if (-not $bad) { $sw2.Add('  (none)') }
foreach ($b in $bad) { $sw2.Add(("  {0,-12} {1}" -f $b.Status, $b.Tu)) }
$sw2.Add('')
$sw2.Add('-' * 100)
$sw2.Add('5. RAW cl output, per TU')
$sw2.Add('-' * 100)
$sw2.AddRange($rawLines)
Set-Content -Path $OutFile -Value ($sw2 -join "`r`n") -Encoding utf8

$json = [pscustomobject]@{
    meta        = @{ generated = (Get-Date -Format o); compiler = $clExe; db = $CompileDb; tus = $tuList.Count;
                     plugin = $(if ($CoreCheckPlugin) { $CoreCheckPlugin } else { $null }) }
    tu_status   = $tuStatus
    diagnostics = $diags
}
Set-Content -Path ([System.IO.Path]::ChangeExtension($OutFile, '.json')) -Value ($json | ConvertTo-Json -Depth 6) -Encoding utf8
Write-Host "[msvc] wrote $OutFile"
Write-Host "[msvc] diagnostics: $($diags.Count) (C26xxx: $($c26999.Count))"