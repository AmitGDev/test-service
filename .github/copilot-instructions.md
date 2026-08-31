# Copilot instructions

## Project overview

`TestService` is intentionally small test infrastructure: a native Windows service used as a predictable target for service-management and diagnostic tooling, not as a production service or application framework.

The executable is built from `src\TestService.cpp` and integrates directly with the Windows Service Control Manager. `ServiceMain` reports `START_PENDING`, waits about five seconds, creates the crash-trigger file, starts the worker, and reports `RUNNING`. Stop and shutdown controls signal the worker, which exits; the service removes the trigger file, waits about five seconds in `STOP_PENDING`, and reports `STOPPED`.

While running, the worker writes a `time: alive` heartbeat every ten seconds to `%ProgramData%\TestService\service.log`. The service creates `delete.to.crash` on the active console user's Desktop. External deletion intentionally crashes the process; the unhandled-exception filter writes `%ProgramData%\TestService\crash.dmp`. Do not change these delays, paths, filenames, or externally observable behaviors without a specific reason, because management and diagnostic tools depend on them.

The implementation uses Win32 service, WTS, Shell, filesystem, and DbgHelp APIs. The worker must remain CRT-aware: it uses `std::jthread` rather than raw `CreateThread` because it calls C++ runtime facilities.

## Build and validation

### Prerequisites

- Windows with an x64 MSVC C++ toolchain.
- CMake 3.25 or newer.
- Ninja.
- LLVM 23.1, providing `clang-format`, `clang-tidy` and `clangd`.
- `clangd`, used by the VS Code C++ language tooling.

`build-x64.ps1` locates the latest installed Visual Studio instance that
provides the MSVC x86/x64 C++ toolchain using `vswhere.exe`. It does not
depend on a specific Visual Studio edition or installation path.

The script initializes a Visual Studio Developer Shell configured for an
x64 host and x64 target.

### Configure and build

The script takes a `-Configuration` parameter (`Debug` by default, or `Release`) and an optional `-Clean` switch that removes the `build\` directory before configuring:

```powershell
.\build-x64.ps1                          # Debug (default)
.\build-x64.ps1 -Configuration Release   # Release
.\build-x64.ps1 -Clean                   # wipe build\ first, then Debug
.\build-x64.ps1 -Configuration Release -Clean
```

Equivalent CMake preset commands are:

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug

cmake --preset x64-release
cmake --build --preset x64-release
```

Both presets use the Ninja generator and write under `build\`, producing `build\x64-debug\bin\TestService.exe` and `build\x64-release\bin\TestService.exe` respectively. `CMakeLists.txt` exports `build\compile_commands.json`, uses C++23 without compiler extensions, and enables high warning levels.

### Formatting and static analysis

CI checks every tracked C++ source/header file with clang-format:

```powershell
<<<<<<< HEAD
clang-format --dry-run --Werror src/*.cpp src/*.hpp
=======
clang-format --dry-run --Werror src\TestService.cpp
>>>>>>> e5adb8a (windows: add copilot-instructions.md)
```

Apply formatting with:

```powershell
<<<<<<< HEAD
clang-format -i src/*.cpp src/*.hpp
=======
clang-format -i src\TestService.cpp
>>>>>>> e5adb8a (windows: add copilot-instructions.md)
```

After configuring with Ninja, run clang-tidy against the generated compile database:

```powershell
<<<<<<< HEAD
clang-tidy -p build src/*.cpp
=======
clang-tidy -p build src\TestService.cpp
>>>>>>> e5adb8a (windows: add copilot-instructions.md)
```

`.clang-tidy` treats warnings as errors and enables broad bug-prone, analyzer, core-guidelines, portability, performance, readability, selected modernize, CERT, and HICPP checks. Use the existing `NOLINT` annotations only for deliberate, localized exceptions. Keep formatting compatible with `.clang-format` (Google-derived style, two-space indentation, 80-column limit).

### Tests

There is no automated test target or test framework in this repository, so there is no single-test command. Validate behavior manually after installing/registering the executable as the `TestService` Windows service:

```powershell
Get-Service TestService
Start-Service TestService
Get-Service TestService
Get-Content "$env:ProgramData\TestService\service.log"
Stop-Service TestService
```

For the crash workflow, start the service, wait until it is `Running`, delete `delete.to.crash` from the active user's Desktop only when intentionally testing a crash, then verify service termination and `%ProgramData%\TestService\crash.dmp`.

## Repository-specific conventions

- Keep behavior deterministic and boring; this repository exists to test other tools.
- Preserve the five-second startup/shutdown delays and ten-second heartbeat unless the change explicitly targets timing behavior.
- Treat service state transitions as asynchronous. The service reports pending states, and callers should wait for the requested state rather than assuming a start/stop request is immediately complete.
- Keep Win32 callback and entry-point signatures compatible with the Service Control Manager. `service_name` is mutable because the Windows API requires `LPWSTR`.
- Use wide Win32 APIs and `std::filesystem::path` for Windows paths. Service artifacts belong under `%ProgramData%\TestService\`; the crash trigger belongs on the interactive user's Desktop, resolved through the active console session.
- Preserve the distinction between normal shutdown and an intentional crash: normal shutdown signals the stop event and removes `delete.to.crash`; external deletion must remain the crash trigger.
- Prefer standard C++ RAII/concurrency facilities where they are compatible with the service APIs. In particular, do not replace the CRT-aware worker thread with raw `CreateThread`.
- Follow the naming enforced by clang-tidy: free functions and methods use `CamelCase`, local variables and parameters use `lower_case`, `constexpr` variables use `kCamelCase`, and namespaces use `lower_case`.
- Keep platform includes grouped as in the source and use `// clang-format off/on` only where required for the Windows include arrangement.
- The only linked Windows libraries currently required are `Advapi32`, `Dbghelp`, `Shell32`, and `Wtsapi32`; update CMake when adding APIs from another library.

## Important files

- `CMakeLists.txt`: target, C++ standard, compiler options, Windows definitions/libraries, output, and install rule.
- `CMakePresets.json`: supported local x64 Debug configure/build preset.
- `build-x64.ps1`: local MSVC environment setup and Debug build.
- `.github\workflows\static-code-analisys.yml`: CI formatting, Debug/Release builds, and clang-tidy procedure.
- `.clang-format` and `.clang-tidy`: enforced formatting and analysis policy.
- `README.md`: service behavior, installation, lifecycle, diagnostic artifacts, and manual test scenarios.
