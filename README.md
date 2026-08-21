# TestService

`TestService` is a small, intentionally simple Windows service used as a development and testing target for service-management and diagnostic tools.

The service itself is not intended to provide useful application functionality. Its purpose is to provide a predictable, controllable Windows service against which tools can be developed and tested.

## Purpose

Developing software that manages Windows services requires a reliable service target on which different operations can be exercised safely.

`TestService` provides such a target.

It can be used to develop and validate tools that perform operations such as:

* Starting a Windows service.
* Stopping a Windows service.
* Restarting a Windows service.
* Querying service state.
* Waiting for service state transitions.
* Detecting unexpected service termination.
* Inspecting service logs.
* Locating and collecting dump files.
* Testing service-management error handling.
* Testing service recovery behavior.
* Testing automation around Windows services.

The implementation is deliberately kept small so that behavior relevant to service management and diagnostics is easy to understand and reproduce.

## What TestService Is

`TestService` is a native Windows service implemented in C++.

The service has two main responsibilities:

1. Integrate with the Windows Service Control Manager.
2. Provide a predictable process, lifecycle, log file, and crash dump that can be used as targets for service-management and diagnostic tools.

The implementation intentionally avoids application-specific business logic.

## Intentional Test Behavior

`TestService` contains several deliberate behaviors that make it useful as a service-management test target.

### Five-Second Start and Stop Delay

The service intentionally simulates a slow startup and shutdown.

During startup it reports `START_PENDING` and waits approximately **5 seconds** before entering `RUNNING`.

During shutdown it reports `STOP_PENDING` and waits approximately **5 seconds** before entering `STOPPED`.

The startup lifecycle is therefore approximately:

```text
Start request
     |
     v
START_PENDING
     |
     | ~5 seconds
     v
RUNNING
````

Shutdown behaves similarly:

```text
Stop request
     |
     v
STOP_PENDING
     |
     | ~5 seconds
     v
STOPPED
```

The delays are intentional. They provide a predictable window in which service-management tools can observe and handle pending service states.

The service log also records the simulated transitions:

```text
2026-08-27T08:31:51.779Z [INFO] Service starting - simulating 5 second startup.
2026-08-27T08:31:56.789Z [INFO] Service startup delay completed.
2026-08-27T08:31:56.791Z [INFO] Created crash trigger file: C:\ProgramData\TestService\delete to crash
2026-08-27T08:31:56.843Z [INFO] Created shortcut: C:\Users\Public\Desktop\TestService.lnk
...
2026-08-27T08:32:17.553Z [INFO] Service stop requested.
2026-08-27T08:32:17.554Z [INFO] Deleted crash trigger file.
2026-08-27T08:32:17.555Z [INFO] Service stopping - simulating 5 second shutdown.
2026-08-27T08:32:22.564Z [INFO] Service shutdown delay completed.
```

This is useful for testing:

* Waiting for service state transitions.
* Polling versus fixed delays.
* Operation timeouts.
* Progress reporting.
* Start/stop synchronization.
* Restart logic.
* Handling a service that does not transition immediately.
* Correlating SCM state transitions with service-side activity.

A management tool should not assume that a successful start or stop request means that the requested state has already been reached.

## Service Log and Crash Dump

`TestService` provides real diagnostic artifacts so that service-management and diagnostic tools can operate against real files rather than simulated test data.

The service creates its working directory under:

```text
%ProgramData%\TestService\
```

The service log is:

```text
%ProgramData%\TestService\service.log
```

The crash dump is:

```text
%ProgramData%\TestService\crash.dmp
```

The resulting layout is:

```text
%ProgramData%\
    TestService\
        service.log
        crash.dmp
```

The implementation deliberately keeps this structure simple. There are no additional logging directories or complex configuration.

### Desktop Shortcut

When the service starts, it creates a shortcut named:

```text
TestService.lnk
```

on the Public Desktop.

The shortcut points to the service data directory:

```text
C:\Users\Public\Desktop\TestService.lnk
                    |
                    v
        %ProgramData%\TestService\
                    |
                    +-- service.log
                    |
                    +-- crash.dmp
```

The shortcut is recreated on every service start.

The shortcut is intentionally **not removed when the service stops or crashes**. It provides convenient access to the service's diagnostic artifacts, including after an unexpected termination.

The shortcut itself has no effect on service operation and can be manually deleted at any time.

Deleting `TestService.lnk` does **not** trigger a service crash.

If the shortcut is deleted while the service is running, the service continues operating normally. The shortcut will be recreated the next time the service starts.

### Service Log

`TestService` writes timestamped diagnostic messages to `service.log`.

Each entry uses an ISO 8601 UTC timestamp:

```text
2026-08-27T09:23:07.795Z [INFO] Service alive.
2026-08-27T09:23:17.800Z [INFO] Service alive.
```

The log records significant service lifecycle events, including:

* Service startup.
* Public Desktop detection.
* Crash-trigger creation and removal.
* Diagnostic shortcut creation.
* Service running state.
* Stop and shutdown requests.
* Service stopping.
* Periodic heartbeat activity.
* Errors encountered during service operation.

### Service Heartbeat

While the service is running, its worker thread periodically writes:

```text
alive
```

to `service.log`.

The default heartbeat interval is **10 seconds**.

For example:

```text
2026-08-27T09:23:07.795Z [INFO] Service alive.
2026-08-27T09:23:17.800Z [INFO] Service alive.
2026-08-27T09:23:27.800Z [INFO] Service alive.
2026-08-27T09:23:37.806Z [INFO] Service alive.
```

The heartbeat provides a simple indication that the service is alive and gives service-management and diagnostic tools a continuously updated, real log file to inspect.

This makes it possible to test functionality such as:

* Reading a service log.
* Monitoring a log for changes.
* Locating service-specific log files.
* Correlating service activity with service state.
* Inspecting artifacts produced by a real service process.
* Correlating lifecycle events with timestamps.

## `delete to crash`

`TestService` uses a file named `delete to crash` as a simple external crash trigger.

When the service starts, it creates the file on the Public Desktop:

```text
C:\Users\Public\Desktop\delete to crash
```

The startup sequence is:

```text
Service starts
     |
     v
Create `delete to crash`
     |
     v
Create `TestService.lnk`
     |
     v
Service running
     |
     +-----------------------------+
     |                             |
     | Normal stop                 | File deleted externally
     |                             |
     v                             v
Delete trigger              Intentional crash
     |                             |
     v                             v
Service exits                    crash.dmp
```

While the service is running:

* If `delete to crash` exists, the service continues running normally.
* If `delete to crash` is deleted externally, the service intentionally terminates.
* During a normal service shutdown, the service removes the trigger file before the process exits.

The crash trigger is deliberately separate from the desktop shortcut.

Deleting the `TestService.lnk` shortcut has no effect on the service.

Only deletion of the `delete to crash` file causes the intentional crash.

The shortcut can be manually deleted at any time without affecting the service. It will be recreated automatically the next time the service starts.

For example, deleting the crash trigger produces:

```text
2026-08-22T18:02:20.467Z [INFO] Crash trigger deleted.
2026-08-22T18:02:20.471Z [ERROR] Service crash triggered.
```

> **Warning:** Do not manually delete `delete to crash` unless you intentionally want to crash `TestService`.

## Mini Dump

`TestService` installs an unhandled-exception handler that writes a full-memory Windows minidump to:

```text
%ProgramData%\TestService\crash.dmp
```

The deleted crash-trigger file deliberately causes an unhandled access violation, which exercises this diagnostic path.

The dump is generated using `MiniDumpWithFullMemory`, providing a real diagnostic artifact for tools that need to test dump discovery, collection, and post-crash diagnostics.

This allows testing of:

* Crash detection.
* Service recovery handling.
* Unexpected process termination.
* Mini-dump generation.
* Dump-file discovery.
* Dump-file collection.
* Correlating the service log with the crash dump.
* Diagnostic workflows involving both logs and dumps.

The service therefore provides a simple but realistic diagnostic environment:

```text
                    TestService
                        |
              +---------+---------+
              |                   |
              v                   v
        service.log          crash.dmp
              |                   |
              +---------+---------+
                        |
                        v
              Diagnostic Tool
```

## Service Lifecycle

The normal lifecycle is managed by the Windows Service Control Manager.

Conceptually:

```text
                 +-------------+
                 |   STOPPED   |
                 +------+------+
                        |
                     Start
                        |
                        v
                 +-------------+
                 |START_PENDING|
                 +------+------+
                        |
                  ~5 seconds
                        |
                        v
                 +-------------+
                 |   RUNNING   |
                 +------+------+
                        |
                      Stop
                        |
                        v
                 +-------------+
                 | STOP_PENDING|
                 +------+------+
                        |
                  ~5 seconds
                        |
                        v
                 +-------------+
                 |   STOPPED   |
                 +-------------+
```

The service reports its state to the Service Control Manager during these transitions.

The service log records the lifecycle independently of the SCM state, allowing a management tool to correlate externally observed service state with activity inside the service process.

The diagnostic shortcut is independent of the service lifecycle. It remains on the Public Desktop after a normal stop or unexpected crash.

This is particularly important for service-management tools because service operations are not necessarily instantaneous.

A management tool should therefore generally treat service control as an asynchronous operation and wait for the requested state rather than assuming that the request itself means the transition has completed.

## Building

`TestService` is built with CMake (minimum 3.25), generating Ninja build files, compiled with `cl.exe` (MSVC), targeting C++23.

### Prerequisites

* Visual Studio (for the MSVC toolchain and `cl.exe`)
* CMake 3.25 or later
* Ninja

### Build presets

Two CMake presets are defined in `CMakePresets.json`:

* `x64-debug`
* `x64-release`

Each configures its own isolated build directory under `build/<preset>/`, so switching between Debug and Release does not require a clean rebuild.

### Local build script

`build-x64.ps1` is the local build entry point:

```powershell
.\build-x64.ps1 -Configuration Debug
```

or:

```powershell
.\build-x64.ps1 -Configuration Release -Clean
```

The script loads the MSVC developer environment automatically, then configures and builds the selected preset.

### Static analysis

clang-format and clang-tidy are configured at the repo root (`.clang-format`, `.clang-tidy`) and run automatically in CI against both presets. They can also be run locally against the generated `compile_commands.json` in `build/<preset>/`.

### Editor support (clangd)

A `.clangd` file at the repo root points clangd at a synced `compile_commands.json`, kept up to date automatically after each local build so that editor diagnostics match what clang-tidy and CI see. No manual setup is required beyond building the project at least once.

## Installation

The service must be registered with the Windows Service Control Manager before it can be started.

The exact installation mechanism may vary depending on the development environment.

After installation, the service should appear as:

```text
TestService
```

in the Windows service database.

It can then be inspected using standard Windows tools such as:

```powershell
Get-Service TestService
```

or:

```cmd
sc query TestService
```

## Running the Service

Once installed, it can be started through the normal Windows service mechanisms.

For example:

```powershell
Start-Service TestService
```

or:

```cmd
sc start TestService
```

Its state can then be inspected with:

```powershell
Get-Service TestService
```

To stop it:

```powershell
Stop-Service TestService
```

or:

```cmd
sc stop TestService
```

These commands are useful not only for manual testing but also as reference behavior when developing a dedicated service-management tool.

## Testing Service Management Tools

A typical test sequence is:

```text
1. Install TestService
2. Verify STOPPED state
3. Start TestService
4. Observe START_PENDING
5. Wait for RUNNING
6. Verify the service process
7. Inspect service.log
8. Verify TestService.lnk exists on the Public Desktop
9. Stop TestService
10. Observe STOP_PENDING
11. Wait for STOPPED
12. Verify process termination
13. Verify TestService.lnk still exists
```

A crash and diagnostic test can be performed as follows:

```text
1. Start TestService
2. Wait for RUNNING
3. Verify `delete to crash` exists on the Public Desktop
4. Verify `TestService.lnk` exists on the Public Desktop
5. Verify service.log contains heartbeat entries
6. Delete `delete to crash`
7. Detect service termination
8. Locate crash.dmp using the diagnostic shortcut
9. Inspect service.log
10. Validate the diagnostic workflow
```

A restart test can verify the complete lifecycle:

```text
TestService
    |
    | start
    v
 START_PENDING
    |
    | ~5 seconds
    v
 RUNNING
    |
    | obtain process information
    v
  PID N
    |
    | stop
    v
 STOP_PENDING
    |
    | ~5 seconds
    v
 STOPPED
    |
    | start
    v
 START_PENDING
    |
    | ~5 seconds
    v
 RUNNING
    |
    v
  PID M
```

A management tool can verify that the second start produces a new service process.

### Shortcut Persistence Test

The shortcut is deliberately persistent across service stops.

A dedicated test can verify this behavior:

```text
1. Start TestService
2. Verify TestService.lnk exists
3. Stop TestService
4. Verify TestService.lnk still exists
5. Start TestService again
6. Verify TestService.lnk exists again
7. Manually delete TestService.lnk
8. Verify the service continues running
9. Stop TestService
10. Start TestService again
11. Verify TestService.lnk is recreated
```

This also verifies that the shortcut itself is not part of the crash-trigger mechanism.

## Error and Race Conditions

Service-management software must account for the fact that service state changes are asynchronous.

For example, this sequence is not necessarily valid:

```text
StartService()
StopService()
```

A start request may have been accepted while the service is still in `START_PENDING`.

A robust management tool should instead observe the service state:

```text
StartService()
    |
    v
START_PENDING
    |
    | wait
    v
RUNNING
```

and only then perform operations that require the service to be running.

Similarly, after requesting a stop:

```text
ControlService(STOP)
    |
    v
STOP_PENDING
    |
    | wait
    v
STOPPED
```

The intentional five-second delays make these transitions easy to reproduce during development and testing.

The ten-second heartbeat and the real log and dump files also make it possible to test diagnostic operations while these state transitions are occurring.

The diagnostic shortcut should not be treated as a service-state indicator. Its presence only indicates that the service has created or previously created the shortcut. It may remain available after the service has stopped or crashed.

## Future Test Behaviors

The service can be extended with additional deliberately controlled behaviors when a management tool needs to test specific scenarios.

Possible future behaviors include:

### Slow Startup

Keep the service in `START_PENDING` for a configurable period.

Useful for testing:

* Startup timeouts.
* Polling.
* Progress reporting.
* Cancellation.

### Slow Shutdown

Keep the service in `STOP_PENDING` for a configurable period.

Useful for testing:

* Stop timeouts.
* Forced termination policies.
* User feedback.

### Hung Service

Stop responding to normal control requests.

Useful for testing:

* Timeout handling.
* Recovery mechanisms.
* Forced termination.
* Diagnostic collection.

### Resource Consumption

Controlled CPU or memory consumption could be added when tools need a predictable process on which to test resource diagnostics.

Any additional behaviors should remain explicitly controlled and deterministic. The default behavior should remain the simple, normal service lifecycle.

## Non-Goals

`TestService` is not intended to be:

* A production Windows service.
* A framework for implementing business services.
* A demonstration of complex application architecture.
* A replacement for a real production service during final validation.
* A general-purpose Windows service template.

Its role is narrower:

> Provide a simple, real Windows service that can safely be used as a target for service-management and diagnostic tools.

## Development Philosophy

The service should remain boring.

That is a feature, not a limitation.

When developing a service-management tool, failures should ideally come from the tool being tested rather than from complicated behavior in the target service.

Therefore, changes to `TestService` should be evaluated based on whether they improve its usefulness as a testing target.

A new feature is justified when it enables testing of a meaningful service-management or diagnostic scenario.

## Repository Role

`TestService` should be considered **test infrastructure**.

It exists to support development of other tools and should therefore favor:

* Stability.
* Deterministic behavior.
* Simple implementation.
* Easy installation.
* Easy removal.
* Easy inspection.
* Repeatable lifecycle operations.
* Realistic diagnostic artifacts.

The value of the project is not in what the service does.

The value is in providing a dependable Windows service that can be started, stopped, restarted, inspected, diagnosed, intentionally crashed, and used as a target while developing service-management and diagnostic software.