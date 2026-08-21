// Win32 SDK types and APIs are intentionally consumed through Windows.h,
// following the Windows SDK's documented usage. Do not add individual SDK
// headers solely to satisfy clang-tidy's include-cleaner diagnostic.
// NOLINTBEGIN(misc-include-cleaner)

// Keep windows.h first. Reordering these Windows SDK headers can cause
// compilation errors.
// clang-format off
#include <windows.h>
// clang-format on

#include <dbghelp.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <syncstream>
#include <system_error>
#include <thread>

#include "scope.hpp"

// A class cannot be given internal linkage with static.
namespace {

class UnknownExceptionErrorCategory final : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override {
    return "unknown_exception";
  }

  [[nodiscard]] std::string message(int /*condition*/) const override {
    return "An unexpected exception was caught";
  }
};

enum class LogLevel : std::uint8_t {
  kInfo,
  kError,
};

// Although std::wstring_view would normally be preferable, these strings
// are passed directly to Windows APIs that require null-terminated strings.
// Raw pointers preserve that representation. Callers must provide valid
// null-terminated strings whose lifetime extends through the call.
struct ShortcutOptions {
  std::reference_wrapper<const std::filesystem::path> directory;
  std::reference_wrapper<const std::filesystem::path> target;
  const wchar_t* file_name = nullptr;
  const wchar_t* description = nullptr;
};

struct ServiceStatusUpdate {
  DWORD state = 0;
  DWORD exit_code = NO_ERROR;
  DWORD wait_hint = 0;
};

}  // namespace

static constexpr std::wstring_view kWindowsStatusFormat = L"0x{:08X}";

// Windows service APIs require a mutable LPWSTR for the service name.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static wchar_t service_name[] = L"TestService";

static constexpr wchar_t kDesktopShortcut[] = L"TestService.lnk";
static constexpr wchar_t kDesktopShortcutDescription[] =
    L"TestService data directory";
static constexpr wchar_t kServiceDirectory[] = L"TestService";
static constexpr wchar_t kCrashTriggerFile[] = L"delete to crash";
static constexpr wchar_t kLogFile[] = L"service.log";
static constexpr wchar_t kDumpFile[] = L"crash.dmp";

static constexpr auto kStartDelay = std::chrono::seconds{5};
static constexpr auto kStopDelay = std::chrono::seconds{5};
static constexpr auto kWorkerInterval = std::chrono::seconds{10};

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
static SERVICE_STATUS_HANDLE g_service_status_handle = nullptr;
static HANDLE g_stop_event = nullptr;
static std::atomic<DWORD> g_service_state{SERVICE_STOPPED};

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

static const std::error_category& GetUnknownExceptionErrorCategory() {
  static const UnknownExceptionErrorCategory category;
  return category;
}

static std::error_code MakeUnknownExceptionErrorCode() {
  return {1, GetUnknownExceptionErrorCategory()};
}

static std::expected<std::filesystem::path, HRESULT>
GetKnownDirectoryPath(REFKNOWNFOLDERID folder_id) noexcept {
  PWSTR path = nullptr;

  const HRESULT result =
      SHGetKnownFolderPath(folder_id, KF_FLAG_DEFAULT, nullptr, &path);

  if (FAILED(result)) {
    constexpr size_t kDiagnosticBufferSize = 128;
    wchar_t buffer[kDiagnosticBufferSize]{};

    try {
      const auto format_result = std::format_to_n(
          std::begin(buffer), std::size(buffer) - 1,
          L"Could not determine known folder path. HRESULT: 0x{:08X}\n",
          static_cast<std::uint32_t>(result));

      *format_result.out = L'\0';
      OutputDebugStringW(std::begin(buffer));
    } catch (...) {
      OutputDebugStringW(
          L"Could not determine known folder path; "
          L"could not format HRESULT.\n");
    }

    return std::unexpected(result);
  }

  if (path == nullptr) {
    OutputDebugStringW(
        L"Could not determine known folder path: API returned a null path.\n");
    return std::unexpected(E_UNEXPECTED);
  }

  // Own the COM-allocated path buffer so it is released automatically on
  // every exit path.
  const auto path_owner =
      std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>{path, &CoTaskMemFree};

  // Constructing std::filesystem::path from a wchar_t* allocates internally
  // and can throw.
  try {
    return std::filesystem::path{path};
  } catch (...) {
    OutputDebugStringW(L"Could not construct known folder path.\n");
    return std::unexpected(E_OUTOFMEMORY);
  }
}

static const std::filesystem::path& GetProgramDataDirectory() noexcept {
  static const std::filesystem::path path =
      [] noexcept -> std::filesystem::path {
    const auto base = GetKnownDirectoryPath(FOLDERID_ProgramData)
                          .value_or(std::filesystem::path{});

    if (base.empty()) {
      OutputDebugStringW(L"Could not obtain ProgramData path.\n");
      return {};
    }

    try {
      return base / kServiceDirectory;
    } catch (...) {
      OutputDebugStringW(L"Could not construct ProgramData path.\n");
      return {};
    }
  }();

  return path;
}

static const std::filesystem::path& GetPublicDesktopDirectory() noexcept {
  static const std::filesystem::path path = [] noexcept {
    auto result = GetKnownDirectoryPath(FOLDERID_PublicDesktop)
                      .value_or(std::filesystem::path{});

    if (result.empty()) {
      OutputDebugStringW(L"Could not obtain public desktop path.");
    }

    return result;
  }();

  return path;
}

// Recreate the directory if it was deleted after startup.
// The path is fixed once at startup, but the directory itself may be deleted
// while the service is running. Recreate it on demand without re-resolving it.
static void EnsureProgramDataDirectory() noexcept {
  std::error_code error;

  // ProgramData already exists, so only the application directory needs to
  // be created. create_directory() is noexcept with an error_code.
  std::filesystem::create_directory(GetProgramDataDirectory(), error);

  if (error) {
    // Log() calls this function, so use the debugger to avoid recursion.
    constexpr size_t kDiagnosticBufferSize = 128;
    wchar_t buffer[kDiagnosticBufferSize];

    try {
      const auto result = std::format_to_n(
          std::begin(buffer), std::size(buffer) - 1,
          L"Could not create ProgramData directory: {}", error.value());
      *result.out = L'\0';
      OutputDebugStringW(std::begin(buffer));
    } catch (...) {
      OutputDebugStringW(
          L"Could not create ProgramData directory (unknown error)\n");
    }
  }
}

// Logging is best-effort and must never propagate an exception to its caller.
static void Log(std::wstring_view message,
                LogLevel level = LogLevel::kInfo) noexcept {
  EnsureProgramDataDirectory();

  try {
    const auto now = std::chrono::floor<std::chrono::milliseconds>(
        std::chrono::system_clock::now());

    static std::wofstream log_stream(GetProgramDataDirectory() / kLogFile,
                                     std::ios::app);

    if (log_stream) {
      std::wosyncstream out(log_stream);
      out << std::format(L"{:%FT%TZ} [{}] ", now,
                         level == LogLevel::kError ? L"ERROR" : L"INFO");
      out << message << L'\n';

      // wosyncstream emits to log_stream on destruction, but log_stream
      // may still buffer the data. Flush here so the entry reaches the
      // underlying stream before Log() returns.
      out.flush();
    }
  } catch (...) {
    OutputDebugStringW(
        L"Log() failed: exception while formatting/writing a log entry.\n");
  }
}

static void LogInfo(std::wstring_view message) noexcept {
  Log(message);
}

static void LogError(std::wstring_view message) noexcept {
  Log(message, LogLevel::kError);
}

static std::expected<void, std::error_code> CreateCrashTriggerFile() noexcept {
  EnsureProgramDataDirectory();

  try {
    const auto path = GetProgramDataDirectory() / kCrashTriggerFile;

    std::ofstream file(path);

    if (!file) {
      return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    file << "Delete this file to crash the service.\n";

    if (!file) {
      return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    LogInfo(L"Created crash trigger file: " + path.wstring());

    return {};
  } catch (const std::bad_alloc&) {
    return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
  } catch (...) {
    // Preserve noexcept while distinguishing unexpected exceptions from
    // failures reported explicitly above.
    return std::unexpected(MakeUnknownExceptionErrorCode());
  }
}

static void DeleteCrashTriggerFile() noexcept {
  // Reasons might be: user removed the directory, memory allocation error, I/O
  // error, etc. If the directory is not accessible, the crash trigger file
  // cannot be accessed.
  if (GetProgramDataDirectory().empty()) {
    return;
  }

  // Path concatenation and format_to_n can throw.
  try {
    std::error_code error;

    if (std::filesystem::remove(GetProgramDataDirectory() / kCrashTriggerFile,
                                error)) {
      LogInfo(L"Deleted crash trigger file.");
    } else if (error) {
      constexpr size_t kDiagnosticBufferSize = 128;
      wchar_t buffer[kDiagnosticBufferSize];

      const auto result = std::format_to_n(
          std::begin(buffer), std::size(buffer) - 1,
          L"Could not delete crash trigger file. Error code: {}",
          error.value());
      *result.out = L'\0';
      LogError(std::begin(buffer));
    }
  } catch (const std::bad_alloc&) {
    LogError(L"Could not delete crash trigger file. Out of memory.");
  } catch (...) {
    LogError(L"Could not delete crash trigger file. Unknown error.");
  }
}

static std::expected<bool, std::error_code> CrashTriggerExists() noexcept {
  // Reasons might be: user removed the directory, memory allocation error, I/O
  // error, etc. If the directory is not accessible, the crash trigger file
  // cannot be accessed.
  if (GetProgramDataDirectory().empty()) {
    return std::unexpected(
        std::make_error_code(std::errc::operation_not_permitted));
  }

  try {
    std::error_code error;

    // Path concatenation can throw.
    const bool exists = std::filesystem::exists(
        GetProgramDataDirectory() / kCrashTriggerFile, error);

    if (error) {
      return std::unexpected(error);
    }

    return exists;
  } catch (const std::bad_alloc&) {
    return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
  } catch (...) {
    return std::unexpected(MakeUnknownExceptionErrorCode());
  }
}

static std::expected<std::filesystem::path, HRESULT>
CreateShortcut(const ShortcutOptions& options) noexcept {
  if (options.directory.get().empty() || options.target.get().empty() ||
      options.file_name == nullptr || *options.file_name == L'\0') {
    return std::unexpected(E_INVALIDARG);
  }

  const HRESULT init_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  if (FAILED(init_result)) {
    return std::unexpected(init_result);
  }

  const auto guard = scope_exit{[] noexcept { CoUninitialize(); }};

  std::filesystem::path shortcut_path;

  try {
    shortcut_path = options.directory.get() / options.file_name;
  } catch (const std::bad_alloc&) {
    return std::unexpected(E_OUTOFMEMORY);
  } catch (...) {
    return std::unexpected(E_FAIL);
  }

  IShellLinkW* shell_link = nullptr;

  HRESULT result =
      CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                       IID_PPV_ARGS(&shell_link));

  if (SUCCEEDED(result)) {
    result = shell_link->SetPath(options.target.get().c_str());
  }

  if (SUCCEEDED(result) && options.description != nullptr) {
    result = shell_link->SetDescription(options.description);
  }

  if (SUCCEEDED(result)) {
    IPersistFile* persist_file = nullptr;

    result = shell_link->QueryInterface(IID_PPV_ARGS(&persist_file));

    if (SUCCEEDED(result)) {
      result = persist_file->Save(shortcut_path.c_str(), TRUE);
      persist_file->Release();
    }
  }

  if (shell_link != nullptr) {
    shell_link->Release();
  }

  if (FAILED(result)) {
    return std::unexpected(result);
  }

  return shortcut_path;
}

static LONG WINAPI
UnhandledExceptionHandler(EXCEPTION_POINTERS* exception) noexcept {
  // The handler is installed only after the fixed directories are initialized,
  // so there is no window in which this handler can observe an empty path.
  EnsureProgramDataDirectory();

  std::filesystem::path dump_path;

  try {
    // Path concatenation can throw bad_alloc.
    dump_path = GetProgramDataDirectory() / kDumpFile;
  } catch (...) {
    OutputDebugStringW(L"Could not construct crash dump path.\n");
    return EXCEPTION_EXECUTE_HANDLER;
  }

  HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

  if (file == INVALID_HANDLE_VALUE) {
    constexpr size_t kDiagnosticBufferSize = 128;
    wchar_t buffer[kDiagnosticBufferSize];

    try {
      const auto result = std::format_to_n(
          std::begin(buffer), std::size(buffer) - 1,
          L"Could not create crash dump file: {}\n", GetLastError());

      *result.out = L'\0';
      OutputDebugStringW(std::begin(buffer));
    } catch (...) {
      OutputDebugStringW(L"Could not create crash dump file.\n");
    }

    return EXCEPTION_EXECUTE_HANDLER;
  }

  MINIDUMP_EXCEPTION_INFORMATION info{};

  info.ThreadId = GetCurrentThreadId();
  info.ExceptionPointers = exception;
  info.ClientPointers = FALSE;

  const BOOL dump_written =
      MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                        MiniDumpWithFullMemory, &info, nullptr, nullptr);

  if (dump_written == FALSE) {
    constexpr size_t kDiagnosticBufferSize = 128;
    wchar_t buffer[kDiagnosticBufferSize];

    try {
      const auto result =
          std::format_to_n(std::begin(buffer), std::size(buffer) - 1,
                           L"MiniDumpWriteDump failed: {}\n", GetLastError());

      *result.out = L'\0';
      OutputDebugStringW(std::begin(buffer));
    } catch (...) {
      OutputDebugStringW(L"MiniDumpWriteDump failed.\n");
    }
  }

  CloseHandle(file);
  return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] static void Crash() noexcept {
  LogError(L"Service crash triggered.");

  // Deliberate access violation, to exercise SetUnhandledExceptionFilter
  // and the minidump path end to end.
  volatile int* invalid_address = reinterpret_cast<volatile int*>(0x1);

  *invalid_address = 42;  // NOLINT(cppcoreguidelines-avoid-magic-numbers)

  std::abort();
}

static void ReportStatus(const ServiceStatusUpdate& update) {
  SERVICE_STATUS status{};
  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = update.state;
  status.dwWin32ExitCode = update.exit_code;
  status.dwWaitHint = update.wait_hint;

  // SCM requires dwControlsAccepted to be 0 while a pending transition is in
  // progress; only SERVICE_RUNNING may accept control requests here, since
  // ServiceControlHandler only reacts to STOP/SHUTDOWN.
  status.dwControlsAccepted =
      update.state == SERVICE_RUNNING
          ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
          : 0;

  // Only pending states advance the checkpoint; SCM treats a stale checkpoint
  // on a pending state as a hung service and may kill it prematurely.
  static DWORD checkpoint = 0;
  const bool is_pending = update.state == SERVICE_START_PENDING ||
                          update.state == SERVICE_STOP_PENDING ||
                          update.state == SERVICE_PAUSE_PENDING ||
                          update.state == SERVICE_CONTINUE_PENDING;
  status.dwCheckPoint = is_pending ? ++checkpoint : 0;

  g_service_state.store(update.state, std::memory_order_release);

  if (SetServiceStatus(g_service_status_handle, &status) == FALSE) {
    const DWORD error = GetLastError();

    try {
      LogError(L"SetServiceStatus failed: " +
               std::format(kWindowsStatusFormat, error));
    } catch (...) {
      LogError(
          L"SetServiceStatus failed; unable to format the Windows error "
          L"code.\n");
    }
  }
}

static void WorkerThread() noexcept {
  // Ensure the service does not wait indefinitely after the worker terminates.
  const auto guard = scope_exit{[] noexcept { SetEvent(g_stop_event); }};

  while (true) {
    // Blocks until g_stop_event is signaled or kWorkerInterval elapses,
    // whichever is first, so one wait serves as both the tick and an
    // interruptible stop check.
    const DWORD result = WaitForSingleObject(
        g_stop_event, static_cast<DWORD>(
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              kWorkerInterval)
                              .count()));

    switch (result) {
      case WAIT_OBJECT_0:
        return;

      case WAIT_TIMEOUT:
        break;

      case WAIT_FAILED:
        try {
          LogError(L"WaitForSingleObject failed: " +
                   std::format(kWindowsStatusFormat, GetLastError()));
        } catch (...) {
          LogError(
              L"WaitForSingleObject failed; unable to format the "
              L"Windows error code.\n");
        }

        return;

      default:
        try {
          LogError(L"Unexpected wait result: " +
                   std::format(kWindowsStatusFormat, result));
        } catch (...) {
          LogError(
              L"Unexpected wait result; unable to format the Windows "
              L"error code.\n");
        }

        return;
    }

    const auto trigger_exists = CrashTriggerExists();

    if (!trigger_exists) {
      try {
        LogError(L"Could not check crash trigger file. Error code: " +
                 std::to_wstring(trigger_exists.error().value()));
      } catch (...) {
        LogError(
            L"Could not check crash trigger file; unable to format "
            L"the error code.\n");
      }
    } else if (!*trigger_exists) {
      LogInfo(L"Crash trigger deleted.");
      Crash();
    }

    LogInfo(L"Service alive.");
  }
}

// The SCM calls ServiceControlHandler for control requests such as
// Stop/Shutdown. ServiceControlHandler translates the SCM control request into
// a stop-event signal; it does not perform the service state transition itself.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): signature is fixed by
// LPHANDLER_FUNCTION_EX; restructuring these params would break the
// RegisterServiceCtrlHandlerExW callback contract.
static DWORD WINAPI
ServiceControlHandler(DWORD control, [[maybe_unused]] DWORD event_type,
                      [[maybe_unused]] void* event_data,
                      [[maybe_unused]] void* context) noexcept {
  // NOLINTEND(bugprone-easily-swappable-parameters)
  if (control != SERVICE_CONTROL_STOP && control != SERVICE_CONTROL_SHUTDOWN) {
    return NO_ERROR;
  }

  if (g_service_state.load(std::memory_order_acquire) != SERVICE_RUNNING) {
    return NO_ERROR;
  }

  if (g_stop_event == nullptr) {
    return ERROR_INVALID_HANDLE;
  }

  LogInfo(control == SERVICE_CONTROL_STOP ? L"Service stop requested."
                                          : L"Service shutdown requested.");

  if (SetEvent(g_stop_event) == FALSE) {
    const DWORD error = GetLastError();

    try {
      LogError(L"SetEvent failed: " + std::format(kWindowsStatusFormat, error));
    } catch (...) {
      LogError(L"SetEvent failed; unable to format the Windows error code.\n");
    }

    return error;
  }

  return NO_ERROR;
}

static bool RegisterServiceHandler() noexcept {
  g_service_status_handle = RegisterServiceCtrlHandlerExW(
      std::data(service_name), ServiceControlHandler, nullptr);

  if (g_service_status_handle != nullptr) {
    return true;
  }

  try {
    OutputDebugStringW((L"RegisterServiceCtrlHandlerExW failed: " +
                        std::format(kWindowsStatusFormat, GetLastError()) +
                        L"\n")
                           .c_str());
  } catch (...) {
    OutputDebugStringW(
        L"RegisterServiceCtrlHandlerExW failed; unable to format "
        L"the Windows error code.\n");
  }

  return false;
}

// Performs the service startup sequence while in SERVICE_START_PENDING.
static std::optional<DWORD> InitializeService() noexcept {
  try {
    LogInfo(std::format(L"Service starting - simulating {} second startup.",
                        kStartDelay.count()));
  } catch (...) {
    LogInfo(L"Service starting - simulating startup.");
  }

  ReportStatus({
      .state = SERVICE_START_PENDING,
      .wait_hint = static_cast<DWORD>(
          std::chrono::duration_cast<std::chrono::milliseconds>(kStartDelay)
              .count()),
  });

  g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  if (g_stop_event == nullptr) {
    const DWORD error = GetLastError();

    try {
      LogError(L"CreateEventW failed: " +
               std::format(kWindowsStatusFormat, error));
    } catch (...) {
      LogError(L"CreateEventW failed; unable to format the error code.");
    }

    return error;
  }

  // Simulates a kStartDelay seconds service startup delay
  // to keep the service in SERVICE_START_PENDING state.
  std::this_thread::sleep_for(kStartDelay);
  LogInfo(L"Service startup delay completed.");

  const auto trigger_result = CreateCrashTriggerFile();

  if (!trigger_result) {
    const auto& error = trigger_result.error();

    try {
      LogError(L"Service startup failed. Error code: " +
               std::to_wstring(error.value()));
    } catch (...) {
      LogError(L"Service startup failed; unable to format the error code.");
    }

    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
    return ERROR_WRITE_FAULT;
  }

  // "Absence" is the expected "continue without a shortcut" path, not a new
  // failure.
  if (!GetPublicDesktopDirectory().empty()) {
    const auto shortcut_result = CreateShortcut({
        .directory = GetPublicDesktopDirectory(),
        .target = GetProgramDataDirectory(),
        .file_name = std::data(kDesktopShortcut),
        .description = std::data(kDesktopShortcutDescription),
    });

    if (shortcut_result) {
      try {
        LogInfo(L"Created shortcut: " + shortcut_result->wstring());
      } catch (...) {
        OutputDebugStringW(
            L"Created shortcut; could not construct log message.\n");
      }
    } else {
      try {
        LogError(
            L"Could not create desktop shortcut. HRESULT: " +
            std::format(kWindowsStatusFormat,
                        static_cast<std::uint32_t>(shortcut_result.error())));
      } catch (...) {
        OutputDebugStringW(
            L"Could not create desktop shortcut; could not "
            L"construct log message.\n");
      }
    }
  }

  return std::nullopt;
}

static bool StartWorker(std::jthread& worker_thread) noexcept {
  try {
    worker_thread = std::jthread(WorkerThread);
    return true;
  } catch (const std::system_error& error) {
    try {
      LogError(L"Failed to create worker thread. Error code: " +
               std::to_wstring(error.code().value()));
    } catch (...) {
      LogError(
          L"Failed to create worker thread; unable to format "
          L"the error code.");
    }

    if (SetEvent(g_stop_event) == FALSE) {
      const DWORD set_event_error = GetLastError();

      try {
        LogError(L"SetEvent failed: " +
                 std::format(kWindowsStatusFormat, set_event_error));
      } catch (...) {
        OutputDebugStringW(
            L"SetEvent failed; unable to format the error code.\n");
      }
    }

    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
    return false;
  }
}

static std::optional<DWORD> WaitForServiceStop() noexcept {
  const DWORD wait_result = WaitForSingleObject(g_stop_event, INFINITE);

  if (wait_result == WAIT_OBJECT_0) {
    return std::nullopt;
  }

  if (wait_result == WAIT_FAILED) {
    const DWORD error = GetLastError();

    try {
      LogError(L"WaitForSingleObject failed: " +
               std::format(kWindowsStatusFormat, error));
    } catch (...) {
      LogError(
          L"WaitForSingleObject failed; unable to format the "
          L"error code.");
    }

    return error;
  }

  try {
    LogError(L"Unexpected wait result: " +
             std::format(kWindowsStatusFormat, wait_result));
  } catch (...) {
    LogError(L"Unexpected wait result; unable to format the error code.");
  }

  return ERROR_INVALID_DATA;
}

// Performs the service state transition and shutdown sequence.
// The stop request was signaled by ServiceControlHandler.
static void ShutdownService(std::jthread& worker_thread) noexcept {
  ReportStatus({
      .state = SERVICE_STOP_PENDING,
      .wait_hint = static_cast<DWORD>(
          std::chrono::duration_cast<std::chrono::milliseconds>(kStopDelay)
              .count()),
  });

  DeleteCrashTriggerFile();

  try {
    LogInfo(std::format(L"Service stopping - simulating {} second shutdown.",
                        kStopDelay.count()));
  } catch (...) {
    LogInfo(L"Service stopping - simulating shutdown.");
  }

  worker_thread.join();

  // Simulates a kStopDelay seconds service shutdown delay
  // to keep the service in SERVICE_STOP_PENDING state.
  std::this_thread::sleep_for(kStopDelay);
  LogInfo(L"Service shutdown delay completed.");

  DeleteCrashTriggerFile();

  CloseHandle(g_stop_event);
  // Do not reset g_stop_event here. The service is now shutting down
  // and will not process further control requests.

  ReportStatus({.state = SERVICE_STOPPED});
}

// The SCM calls ServiceMain for the service's lifetime startup.
// ServiceMain is the orchestrator and decides when initialization is completely
// successful.
static void WINAPI ServiceMain([[maybe_unused]] DWORD argc,
                               [[maybe_unused]] LPWSTR* argv) noexcept {
  if (!RegisterServiceHandler()) {
    return;
  }

  if (const auto error = InitializeService()) {
    ReportStatus({.state = SERVICE_STOPPED, .exit_code = *error});
    return;
  }

  std::jthread worker_thread;

  if (!StartWorker(worker_thread)) {
    ReportStatus(
        {.state = SERVICE_STOPPED, .exit_code = ERROR_NOT_ENOUGH_MEMORY});
    return;
  }

  ReportStatus({.state = SERVICE_RUNNING});

  if (const auto error = WaitForServiceStop()) {
    // The wait failed, so there is no stop signal to wake the worker.
    // Signal it (best-effort) before joining; otherwise join() could
    // block indefinitely while the worker waits on g_stop_event.
    SetEvent(g_stop_event);

    // The worker may still be using g_stop_event, so join before closing it.
    // jthread would join automatically on destruction, but that is too late.
    worker_thread.join();

    CloseHandle(g_stop_event);
    g_stop_event = nullptr;

    ReportStatus({.state = SERVICE_STOPPED, .exit_code = *error});
    return;
  }

  ShutdownService(worker_thread);
}

// NOLINTNEXTLINE(misc-use-internal-linkage,readability-identifier-naming)
int wmain() noexcept {
  // Resolve and create the mandatory ProgramData directory before
  // installing the exception filter or starting the service dispatcher.
  // The service cannot operate without this directory because logging,
  // crash dumps, trigger files, and other artifacts depend on it.
  // If the directory is later removed, it can be recreated using the
  // already-resolved path; resolving the known folder again is unnecessary.
  if (const auto& result = GetProgramDataDirectory(); result.empty()) {
    return static_cast<int>(ERROR_PATH_NOT_FOUND);
  }

  SetUnhandledExceptionFilter(UnhandledExceptionHandler);

  SERVICE_TABLE_ENTRYW service_table[] = {
      {
          .lpServiceName = std::data(service_name),
          .lpServiceProc = ServiceMain,
      },
      {
          .lpServiceName = nullptr,
          .lpServiceProc = nullptr,
      },
  };

  if (StartServiceCtrlDispatcherW(std::data(service_table)) == FALSE) {
    return static_cast<int>(GetLastError());
  }

  return 0;
}

// NOLINTEND(misc-include-cleaner)