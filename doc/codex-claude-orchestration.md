# Codex-Orchestrated Claude Worker

Status: verified tooling note. This describes how an external codex session drives
Claude Code as a non-interactive worker in this repository. It does not change the
runtime architecture; see [hand-input-architecture.md](hand-input-architecture.md)
for that.

## Roles

```text
codex        orchestrator
             task decomposition, sequencing, review of returned reports

claude -p    worker
             repository reading, edits, native/managed builds, tests
```

The worker has no human in the loop. Every run starts with a fresh context, so the
task text must carry everything the worker cannot derive from the repository.

## Entry point

```powershell
tools\codex-claude-task.ps1 <task> [options]
```

`<task>` is either literal task text or a path to a task file. A file is preferred
for long tasks; literal text is limited by the Windows command line.

Options:

```text
-NativeBuildEnv    Enter the VS 2026 ARM64 developer environment first.
                   Required for any cmake/ninja/cl.exe work.
-Model             Worker model. Default opus.
-OutputFormat      text | json | stream-json. Default json.
-SessionId <uuid>  Fixed session id so the run can be continued later.
-Resume            Continue -SessionId instead of starting it.
-MaxBudgetUsd      Hard spend ceiling for one run. Default 5.
-Unrestricted      Add --dangerously-skip-permissions. Not the default.
-Async             Start the worker detached and return a status object at once.
-LogPath           Log file for -Async. Defaults to tmp\codex-worker-<stamp>.log.
```

The wrapper always runs from the repository root and appends repository guardrails
to the worker's system prompt.

## Examples

Documentation/consistency task, no build:

```powershell
powershell -NoProfile -File tools\codex-claude-task.ps1 `
  -OutputFormat json `
  -Task "doc/native-vision-runtime.md の ABI v19 記述と include/ryoiki_native.h の実装を突き合わせ、差異のみ報告する。ファイルは変更しない。"
```

Native build and test task:

```powershell
powershell -NoProfile -File tools\codex-claude-task.ps1 -NativeBuildEnv `
  -OutputFormat json `
  -Task tmp\codex-task.md
```

Multi-turn worker driven across several codex steps:

```powershell
$sid = [guid]::NewGuid().ToString()
powershell -NoProfile -File tools\codex-claude-task.ps1 -SessionId $sid -Task "..."
powershell -NoProfile -File tools\codex-claude-task.ps1 -SessionId $sid -Resume -Task "..."
```

## Required codex configuration

Two codex-side settings are mandatory. Both were found by running the real
end-to-end path, and each fails in a way that looks like a worker bug.

### 1. Sandbox network access

Codex's default `workspace-write` sandbox blocks network access. The spawned worker
then **hangs silently** — no error message, no timeout of its own, just a `claude`
process sitting at near-zero CPU until codex's own timeout kills it.

Enable it per run:

```powershell
codex exec -c sandbox_workspace_write.network_access=true ...
```

Or persistently in `~/.codex/config.toml`:

```toml
[sandbox_workspace_write]
network_access = true
```

The codex banner must show `(network access enabled)`. With it enabled, the same
worker call that hung completed in 5.9 seconds.

### 2. The codex shell timeout

This is the second thing that must be configured on the codex side. Codex's shell tool
defaults to a **10 second** command timeout, and a worker run takes far longer, so a
naive call always fails with:

```text
command timed out after 10228 milliseconds
```

Two supported ways around it.

**1. Tell codex to extend the timeout.** The shell tool takes a `timeout_ms`
parameter, so the orchestrator prompt must ask for it explicitly:

```text
Run this command with your shell tool and set timeout_ms to 900000, because the
command takes much longer than the default timeout:

powershell -NoProfile -File tools\codex-claude-task.ps1 -OutputFormat json -Task '...'
```

**2. Use `-Async` and poll.** This does not depend on any codex timeout setting and
is the better choice for native builds, which can run for many minutes. The wrapper
prints a status object and returns immediately:

```json
{"status":"started","pid":9432,
 "log_path":"...\\tmp\\codex-worker-20260730-224613.log",
 "done_path":"...\\tmp\\codex-worker-20260730-224613.log.done",
 "poll":"..."}
```

The orchestrator then runs the returned `poll` command, which prints `running`
until the worker finishes and afterwards prints `exit=<code>` followed by the full
worker output. The worker runs in a detached process, so it survives the shell call
that started it.

`tmp\codex-worker-*` is git-ignored.

## Parsing the result

With `-OutputFormat json` the worker emits one JSON object. The fields codex should
read are:

```text
result              final report text
is_error            true when the run failed
subtype             success | error_max_turns | error_during_execution
session_id          reuse with -Resume
permission_denials  commands the worker was not allowed to run
total_cost_usd      cost of that run
num_turns           agent turns consumed
```

Treat a non-empty `permission_denials` array as a configuration problem, not as a
worker failure: the allow list in `.claude/settings.json` needs the command, or the
task should not have required it.

## Orchestrator prompt template

Paste this shape into codex. It carries both required settings:

```text
Delegate the following task to the Claude worker and then review its report.

Use your shell tool with timeout_ms set to 900000, and run exactly:

powershell -NoProfile -File tools\codex-claude-task.ps1 -NativeBuildEnv `
  -OutputFormat json -Task 'TASK TEXT HERE'

The command prints one JSON object. Read its "result" field as the worker's
report and its "permission_denials" field. Do not re-run the task yourself.
```

For a long build, replace the direct call with `-Async` and poll the returned
`poll` command instead.

Note that PowerShell 5.1 mangles native arguments containing embedded double
quotes. Quote the `-Task` value with single quotes, or pass a task file path.

## Measured worker behavior

These were verified against Claude Code 2.1.220 on this machine, not assumed:

- **`AGENTS.md` is not loaded into the worker's context automatically.** The wrapper's
  appended system prompt tells the worker to read it. For architectural tasks, name
  the specific documents in the task text as well.
- **The worker exposes a Bash tool, not a PowerShell tool.** Write task text and
  allow-list entries against Bash. The developer environment set by `-NativeBuildEnv`
  reaches Bash through inherited environment variables, so `cmake`, `ninja`, and
  `ctest` work.
- **A command outside the allow list is denied immediately, not queued for approval.**
  The run continues; it does not hang. Widen `.claude/settings.json` instead of
  expecting an approval prompt.
- **The worker's Bash sandbox rejects shell variable expansion** such as
  `echo "$VAR"`. Use `printenv VAR`.
- `-NativeBuildEnv` may print `'vswhere.exe' is not recognized ...` on stderr from
  inside `Enter-VsDevShell` while still configuring the environment correctly.
  Verify the environment with `VSCMD_ARG_TGT_ARCH`, not with stderr being empty.

## Permissions

`.claude/settings.json` holds the worker's allow and deny lists. It is project
configuration, intentionally committed so orchestrated runs behave identically for
every contributor.

The deny list exists for reasons specific to this repository:

```text
dotnet run             the WPF app is a GUI process that never exits and would
RyoikiTenkai.Wpf.exe   hang a non-interactive run
git commit / git push  history is the human's decision
git reset --hard       destroys uncommitted work in a tree that usually has it
git clean
```

Camera-dependent verification therefore cannot be delegated to the worker. Runtime
checks that need the live camera, the native viewer, or the 3D plot stay manual.

`-Unrestricted` bypasses these checks. Use it only for a task that genuinely needs
a command outside the allow list, and prefer adding that command to the allow list
instead.

## What to delegate

Good worker tasks:

```text
document/implementation consistency audits
native or WPF build and ctest runs
adding tests around existing native measurement or recognition code
mechanical refactors inside one responsibility directory
log and CSV analysis under tmp/ or gesture-recordings/
```

Keep with the human or the orchestrator:

```text
live camera and native viewer verification
ABI version decisions
architectural boundary changes
commits, pushes, and branch policy
threshold tuning that requires watching real hand motion
```
