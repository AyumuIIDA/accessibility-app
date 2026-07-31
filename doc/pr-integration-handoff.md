# PR #1 integration handoff

Updated: 2026-08-01

Workspace: `C:\Users\hyena\accessibility-app`

Local branch/base: `main` / `252d456d487e6504e43d0be5b6f17ec611129b0b`

Reference PR head: `origin/pr/1/head` / `a5bed239e63044f0eb4393ac19084ae9258df16f` (`airdrop`)

## 1. Objective and architectural decision

The task was to port the functional intent of PR #1 into the more mature local C++ platform without replacing the local architecture with the PR's C# implementation.

The resulting boundary is:

```text
C++ native runtime
  perception -> measurements -> recognition -> ordered events
  DTW and registration data processing
  SQLite template/recording/binding persistence
  native GPU debug and recording playback surfaces

C# / WPF
  UI and workflow
  command binding and OS action execution
  LAN handoff orchestration
  polling small native metadata structures
```

Continue to follow `doc/hand-input-architecture.md`. In particular, do not move landmark arrays, DTW calculation, image processing, or high-frequency rendering into WPF.

This is a functional integration, not a file-for-file merge. PR binaries, published output, vendored packages, and superseded C# implementations were intentionally not copied.

## 2. Implemented PR scope

### Gesture feature construction and DTW

- One-hand dynamic gesture comparison is implemented natively.
- Two-hand comparison uses a 132-dimensional frame:
  - 64 features per hand
  - 4 relative-hand features
- Two-hand ordering follows the PR intent:
  - handedness ordering when the handedness confidence gap is reliable
  - palm center X fallback otherwise
- Two hands are paired from the same observation timestamp; no synthetic interpolation join was introduced.
- Registration and live recognition enforce confidence, coverage, topology, duration, and sample-count gates corresponding to the PR behavior.
- Bounded L1 DTW, reverse comparison, relative-motion gates, and live candidate windows are implemented.
- Recognition stabilization includes consecutive-match confirmation, cooldown, and duplicate-segment suppression.

Relevant native code:

- `src/RyoikiTenkai.Native/src/HandInput/Measurements/hand_unified_feature_frame.*`
- `src/RyoikiTenkai.Native/src/HandInput/Measurements/gesture_candidate_window_history.*`
- `src/RyoikiTenkai.Native/src/HandInput/Recognition/unified_sequence_gesture_comparator.*`
- `src/RyoikiTenkai.Native/src/HandInput/Recognition/multi_hand_gesture_core.*`
- `src/RyoikiTenkai.Native/src/HandInput/Recognition/gesture_match_stabilizer.*`

The inspected PR head does not contain an operational static-gesture recognition path. Its recognizer and candidate flow are dynamic; therefore static gestures are not an omitted integration item.

### Registration and persistence

- Registration records three takes with user-visible one-based trial numbering.
- Templates and source recording provenance are committed transactionally to SQLite.
- Recording provenance includes capture time, source ID, duration, average confidence, quality counters, and versioned frame payload.
- Both one-hand and two-hand normalized 21-landmark skeleton sequences are retained natively.
- SQLite migrations cover existing databases.
- Gesture definitions and command bindings are also persisted.

Relevant code:

- `src/RyoikiTenkai.Native/src/HandInput/Recognition/gesture_recording_session.*`
- `src/RyoikiTenkai.Native/src/HandInput/Recognition/gesture_template_repository.*`
- `src/RyoikiTenkai.Native/src/HandInput/Recognition/gesture_template_serialization.*`

### Review and debugging

- Saved takes can be listed, selected, played, paused, sought, and exported.
- Playback uses a native child GPU surface. Landmark frame arrays do not cross the public C ABI.
- The DTW debugger is native-rendered and exposes sequence alignment/cost information and hand-oriented visualization.
- WPF owns only the windows and controls around those native surfaces.

Relevant code:

- `src/RyoikiTenkai.Native/src/gesture_dtw_debug_runtime.cpp`
- `src/RyoikiTenkai.Native/src/gesture_recording_playback_runtime.cpp`
- `src/RyoikiTenkai.Native/src/Rendering/gesture_dtw_debug_*`
- `src/RyoikiTenkai.Wpf/GestureDtwDebugWindow.xaml*`
- `src/RyoikiTenkai.Wpf/GestureRecordingPlaybackWindow.xaml*`
- `src/RyoikiTenkai.Wpf/Native/GestureRecordingPlaybackHost.cs`

### Recognition to command path

- WPF consumes the native ordered gesture-event cursor rather than treating the latest recognition snapshot as an event stream.
- Definition-ID bindings are cached and dispatched through `GestureCommandDispatcher` and `IActionExecutor`.
- Supported actions include keyboard input, text input, application launch, `handoff.grab`, and `handoff.release`.
- Failed actions are not automatically retried when retrying could duplicate a dangerous external effect.

Relevant code:

- `src/RyoikiTenkai/Core/GestureCommandBinding.cs`
- `src/RyoikiTenkai/Core/GestureCommandDispatcher.cs`
- `src/RyoikiTenkai/Actions/ActionExecutor.cs`

### LAN handoff

Implemented transport properties:

- UDP offer discovery
- 30-second offer TTL
- one-time authenticated claim
- TCP payload transfer
- random 256-bit credential and fixed-time verification
- 16 MiB payload limit and 4096-byte protocol-line limit
- destination path traversal protection
- cancellation and bounded shutdown

Relevant code and documentation:

- `src/RyoikiTenkai/Actions/HandoffService.cs`
- `src/RyoikiTenkai/Actions/HandoffPayload.cs`
- `src/RyoikiTenkai.Wpf/Interaction/LanGestureValidationServer.cs`
- `doc/lan-handoff.md`

## 3. LAN handoff is gesture-driven

The reference PR has no LAN controls at all: `handoff.grabScreenshot` and `handoff.releaseHere` appear only in the gesture action list, and the transport advertises and claims automatically. The local build previously carried temporary `Offer Screen` and `Receive Offer` buttons for transport validation; those have been removed so behaviour matches the PR.

The implemented flow is:

```text
recognized gesture bound to handoff.grab
  -> capture and retain a payload
  -> advertise it automatically on the LAN
  -> show passive status/available receiver information
  -> handoff.release on the receiving device
  -> authenticated claim and transfer
  -> display/save received payload
```

Current state:

1. `HandoffState` (`Idle`, `Advertising`, `OfferAvailable`, `Claiming`, `Completed`, `Failed`) is defined in `src/RyoikiTenkai/Actions/HandoffPayload.cs` and derived by `HandoffService.GetStatus()`.
2. State transitions come only from ordered gesture commands and from transport progress. No main-screen control can advance them.
3. The manual Offer/Receive buttons and their handlers are gone. Do not reintroduce them, including behind a debug flag, without first re-reading `doc/lan-handoff.md`.
4. `HandoffStatusText` in the main window renders phase, peer name, filename, remaining TTL, and failure reasons; it is refreshed by `NativePollTimer_Tick` before the native early-return so offers and expiry stay visible during camera warm-up.
5. `handoff.release` with no unexpired offer fails visibly instead of selecting an arbitrary device. Choosing between several simultaneous offers is still unimplemented; the newest unexpired offer wins.
6. The authenticated transport and its limits were not modified.

Remaining UX gaps: no in-app cancellation of an active offer (it can only expire by TTL), and no peer picker when multiple offers are visible.

## 4. ABI and data ownership

The current native ABI is **29** in both:

- `src/RyoikiTenkai.Native/include/ryoiki_native.h`
- `src/RyoikiTenkai.Wpf/Native/NativeVisionInterop.cs`

Playback metadata has explicit native/managed layout coverage. Do not add or reorder exported fields without bumping the ABI and extending `ryoiki_native_abi_tests.cpp`.

Ownership rules to preserve:

- Native owns image, tensor, template-frame, recording-frame, and render buffers.
- C# polls copied fixed-size metadata.
- Native playback reads recordings through the native repository/shared access path.
- Playback and debugger child HWND/native handles must be destroyed before the main native runtime stops.

## 5. Verification status

Last verified on 2026-08-01:

```powershell
$buildCommand = 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=arm64 -host_arch=arm64 && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "C:\Users\hyena\accessibility-app\build\RyoikiTenkai.Native.Qnn.Arm64" --config Debug'
& $env:ComSpec /d /s /c $buildCommand

& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' `
  --test-dir build/RyoikiTenkai.Native.Qnn.Arm64 -C Debug --output-on-failure

dotnet build src/RyoikiTenkai.Wpf/RyoikiTenkai.Wpf.csproj --no-restore -p:Platform=ARM64

# RyoikiTenkai.Managed.Tests is a console assertion program, not a VSTest project.
# `dotnet test` silently reports nothing for it; build and run the executable.
dotnet build tests/RyoikiTenkai.Managed.Tests/RyoikiTenkai.Managed.Tests.csproj
& 'tests/RyoikiTenkai.Managed.Tests/bin/Debug/net10.0-windows10.0.26100.0/RyoikiTenkai.Managed.Tests.exe'

git diff --check
```

Results:

- Native ARM64/QNN build: passed
- CTest: 12/12 passed
- WPF ARM64 build: passed
- Managed tests: passed
- `git diff --check`: passed; line-ending notices only
- WPF build may emit `NU1900` when the NuGet vulnerability feed is unreachable. This was not a compilation failure.

The gesture-only LAN change re-ran the managed half only, since it touched no native code:

- WPF ARM64 build: passed, 0 warnings
- Managed tests: passed
- Native build and CTest: not re-run

## 6. Required real-device acceptance pass

Automated tests do not replace the following camera/device checks:

1. Register a one-hand gesture for all three takes.
2. Register a two-hand gesture for all three takes, including ROI overlap and brief occlusion.
3. Restart the application and confirm definitions, three takes, provenance, and bindings persist.
4. Open Review and verify take metadata, play/pause, seek, GPU rendering, and export, including a non-ASCII path.
5. Run `Test it now`; confirm DTW visualization updates and does not remain incorrectly at `Not yet`.
6. Confirm consecutive/cooldown/duplicate suppression produces one intended ordered event.
7. Bind a safe action and verify it executes once.
8. On two machines on the same Wi-Fi, validate discovery, TTL expiry, one-time claim, successful transfer, cancellation, and shutdown.
9. Validate the gesture-driven `handoff.grab`/`handoff.release` path end to end: bind both actions to registered gestures, confirm the main-window status moves `Idle -> Advertising -> OfferAvailable -> Claiming -> Completed`, and confirm a release gesture with no unexpired offer reports `Failed` instead of transferring.

Capture logs for false rejection, incorrect two-hand ordering, dropped ordered events, action duplication, and LAN claim failures.

## 7. Working-tree and collaboration cautions

The working tree contains many modified and untracked files from the broader ongoing development effort. They are not all created by this PR integration task. Do not clean, reset, or mass-format the tree. Review and commit by logical feature group.

Suggested commit groups:

1. Native gesture measurements/DTW/recognition tests
2. SQLite registration, provenance, and ABI
3. Native debugger and recording playback
4. WPF registration/review UI
5. Binding/action event path
6. LAN transport and tests
7. LAN gesture-only status model and button removal
8. Documentation

The repository Claude wrapper is documented in `doc/codex-claude-orchestration.md`. It was invoked during the final audit, but the external Claude API returned connection refusal or hung without output. No Claude audit result should be assumed to exist. Subagents and direct source comparison were used instead.

## 8. Definition of done for the remaining work

The PR integration can be treated as complete when:

- the real-device acceptance pass above succeeds;
- LAN transfer stays gesture-driven with no manual Offer/Receive controls;
- no regression appears in single-hand tracking after target loss/reacquisition;
- two-hand recognition remains stable through practical crossing/partial occlusion cases;
- all native and managed tests remain green;
- any ABI or setup changes are reflected in `README.md` and the relevant `doc/` design files.
