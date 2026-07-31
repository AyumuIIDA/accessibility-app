<#
.SYNOPSIS
    Runs Claude Code as a non-interactive worker for an external orchestrator such as codex.

.DESCRIPTION
    Wraps `claude -p` with this repository's guardrails:
      - optionally enters the Visual Studio ARM64 developer environment so native
        CMake/Ninja builds work inside the worker;
      - always runs from the repository root;
      - appends repository-specific instructions to the worker's system prompt;
      - returns structured JSON by default so the orchestrator can parse the result.

    The orchestrator supplies the task either as a literal string or as a path to a
    task file. Argument length is limited by the Windows command line (~32000 chars);
    use a task file for anything longer.

.EXAMPLE
    tools\codex-claude-task.ps1 "doc/native-vision-runtime.md の ABI v19 節を実装と突き合わせて差異を報告"

.EXAMPLE
    tools\codex-claude-task.ps1 tmp\codex-task.md -NativeBuildEnv -OutputFormat stream-json
#>
[CmdletBinding()]
param(
    # Task text, or a path to a file containing the task text.
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $Task,

    # Enter the VS 2026 ARM64 developer environment before running the worker.
    # Required for any task that configures or builds the native runtime.
    [switch] $NativeBuildEnv,

    [string] $Model = 'opus',

    [ValidateSet('text', 'json', 'stream-json')]
    [string] $OutputFormat = 'json',

    # Reuse a caller-chosen UUID so the orchestrator can continue this worker later
    # with -Resume.
    [string] $SessionId,

    # Continue the conversation identified by -SessionId instead of starting it.
    [switch] $Resume,

    # Hard spend ceiling for one worker run.
    [string] $MaxBudgetUsd = '5',

    # Skip all permission checks. Only for fully trusted orchestration; the default
    # relies on .claude/settings.json plus acceptEdits.
    [switch] $Unrestricted,

    # Start the worker detached, print a status object immediately, and let the
    # orchestrator poll the log file. Use this when the caller's shell tool has a
    # short command timeout.
    [switch] $Async,

    # Log file for -Async. Defaults to tmp\codex-worker-<timestamp>.log.
    [string] $LogPath
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot

$taskFile = $null
if ($Task.Length -lt 260 -and $Task -notmatch '[\r\n]') {
    try {
        if (Test-Path -LiteralPath $Task -PathType Leaf) { $taskFile = $Task }
    }
    catch {
        $taskFile = $null
    }
}

if ($taskFile) {
    $taskText = Get-Content -LiteralPath $taskFile -Raw
}
else {
    $taskText = $Task
}

if ([string]::IsNullOrWhiteSpace($taskText)) {
    throw 'The task text is empty.'
}

if ($NativeBuildEnv -and $env:VSCMD_ARG_TGT_ARCH -ne 'arm64') {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "vswhere.exe was not found at $vswhere."
    }

    $vsPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 `
        -property installationPath

    if (-not $vsPath) {
        throw 'Visual Studio 2026 with the ARM64 C++ tools was not found.'
    }

    Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -Arch arm64 -HostArch arm64 | Out-Null
    $env:VCPKG_ROOT = Join-Path $vsPath 'VC\vcpkg'
}

Set-Location -LiteralPath $repoRoot

$guardrails = @'
You are running as a non-interactive worker started by an external orchestrator
(codex). There is no human in the loop during this run.

Repository rules:
- Read AGENTS.md before any architectural or code change. It is binding.
- doc/hand-input-architecture.md is the source of truth for the
  Observation -> Measurement -> Recognition -> Interaction -> Command boundary.
- doc/native-vision-runtime.md is the source of truth for the current C ABI.
- Keep heavy per-frame vision work in C++. Do not move it into C#.

Execution rules for this mode:
- Never run `dotnet run` and never launch RyoikiTenkai.Wpf.exe. It is a GUI
  process that does not exit, and it will hang this run.
- Do not start background servers or leave any long-running process behind.
- Do not run `git commit` or `git push` unless the task text explicitly asks.
- Native builds require the ARM64 developer environment. If cl.exe or cmake is
  missing, report that instead of building with a mismatched toolchain.
- Prefer the Bash tool for build/test commands; the developer environment is
  inherited through environment variables.

Reporting rules:
- End with a short factual report: what changed, what was built, what was
  tested, and what you did not do. Include failing output verbatim.
- Do not claim a build or test passed unless you ran it in this session.
'@

$claudeArgs = @(
    '-p', $taskText,
    '--model', $Model,
    '--output-format', $OutputFormat,
    '--append-system-prompt', $guardrails,
    '--max-budget-usd', $MaxBudgetUsd
)

if ($Unrestricted) {
    $claudeArgs += '--dangerously-skip-permissions'
}
else {
    $claudeArgs += @('--permission-mode', 'acceptEdits')
}

if ($SessionId) {
    if ($Resume) {
        $claudeArgs += @('--resume', $SessionId)
    }
    else {
        $claudeArgs += @('--session-id', $SessionId)
    }
}
elseif ($Resume) {
    throw '-Resume requires -SessionId.'
}

if (-not $Async) {
    & claude @claudeArgs
    exit $LASTEXITCODE
}

if (-not $LogPath) {
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
    $LogPath = Join-Path $repoRoot ("tmp\codex-worker-$stamp.log")
}

$logDir = Split-Path -Parent $LogPath
if (-not (Test-Path -LiteralPath $logDir)) {
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
}

$donePath = "$LogPath.done"
foreach ($stale in @($LogPath, $donePath)) {
    if (Test-Path -LiteralPath $stale) { Remove-Item -LiteralPath $stale -Force }
}

# A detached process is required: the caller's shell tool returns immediately, so a
# child tied to this session (Start-Job) would be killed. Arguments go through a
# JSON file because Start-Process does not quote array elements containing spaces.
$claudePath = (Get-Command claude).Source
$argsPath = "$LogPath.args.json"
$launcherPath = "$LogPath.launcher.ps1"

ConvertTo-Json -InputObject @($claudeArgs) |
    Set-Content -LiteralPath $argsPath -Encoding UTF8

$launcher = @"
`$ErrorActionPreference = 'Continue'
Set-Location -LiteralPath '$repoRoot'
`$claudeArgv = @(Get-Content -LiteralPath '$argsPath' -Raw -Encoding UTF8 | ConvertFrom-Json)
& '$claudePath' @claudeArgv 2>&1 | Out-File -LiteralPath '$LogPath' -Encoding utf8
Set-Content -LiteralPath '$donePath' -Value `$LASTEXITCODE -Encoding ascii
"@

Set-Content -LiteralPath $launcherPath -Value $launcher -Encoding UTF8

$proc = Start-Process -FilePath 'powershell.exe' `
    -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$launcherPath`"") `
    -WorkingDirectory $repoRoot -WindowStyle Hidden -PassThru

[pscustomobject]@{
    status    = 'started'
    pid       = $proc.Id
    log_path  = $LogPath
    done_path = $donePath
    poll      = "if (Test-Path -LiteralPath '$donePath') { 'exit=' + (Get-Content -LiteralPath '$donePath' -Raw).Trim(); Get-Content -LiteralPath '$LogPath' -Raw } else { 'running' }"
} | ConvertTo-Json -Compress

exit 0
