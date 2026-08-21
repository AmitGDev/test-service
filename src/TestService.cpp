// NOLINTBEGIN(misc-include-cleaner,llvm-include-order)

// clang-format off
#include <windows.h>
#include <dbghelp.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
// clang-format on

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <syncstream>
#include <system_error>
#include <thread>

namespace {

constexpr std::wstring_view kWindowsStatusFormat = L"0x{:08X}";

// Windows service APIs require a mutable LPWSTR for the service name.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
wchar_t service_name[] = L"TestService";

constexpr wchar_t kDesktopShortcut[] = L"TestService.lnk";
constexpr wchar_t kServiceDirectory[] = L"TestService";
constexpr wchar_t kCrashTriggerFile[] = L"delete to crash";
constexpr wchar_t kLogFile[] = L"service.log";
constexpr wchar_t kDumpFile[] = L"crash.dmp";

constexpr auto kStartDelay = std::chrono::seconds{5};
constexpr auto kStopDelay = std::chrono::seconds{5};
constexpr auto kWorkerInterval = std::chrono::seconds{10};

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
SERVICE_STATUS_HANDLE g_service_status_handle = nullptr;
HANDLE g_stop_event = nullptr;
std::atomic<DWORD> g_service_state{SERVICE_STOPPED};

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

const std::filesystem::path& ProgramDataDirectory() {
  static const auto path = [] {
    PWSTR program_data = nullptr;

    const HRESULT result =
        SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &program_data);

    if (FAILED(result) || program_data == nullptr) {
      return std::filesystem::path{};
    }

    auto result_path = std::filesystem::path(program_data) / kServiceDirectory;

    CoTaskMemFree(program_data);

    return result_path;
  }();

  return path;
}

void EnsureProgramDataDirectory() {
  if (ProgramDataDirectory().empty()) {
    return;
  }

  std::error_code error;
  std::filesystem::create_directories(ProgramDataDirectory(), error);
}

enum class LogLevel : std::uint8_t {
  kInfo,
  kError,
};

void Log(const std::wstring& message, LogLevel level = LogLevel::kInfo) {
  EnsureProgramDataDirectory();

  const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());

  static std::wofstream log_stream(ProgramDataDirectory() / kLogFile, std::ios::app);

  if (log_stream) {
    std::wosyncstream out(log_stream);
    out << std::format(L"{:%FT%TZ} [{}] ", now, level == LogLevel::kError ? L"ERROR" : L"INFO");
    out << message << L'\n';
  }

  log_stream.flush();
}

void LogInfo(const std::wstring& message) {
  Log(message);
}

void LogError(const std::wstring& message) {
  Log(message, LogLevel::kError);
}

const std::filesystem::path& PublicDesktopDirectory() {
  static const auto path = [] {
    PWSTR public_desktop = nullptr;

    const HRESULT result =
        SHGetKnownFolderPath(FOLDERID_PublicDesktop, KF_FLAG_DEFAULT, nullptr, &public_desktop);

    if (FAILED(result) || public_desktop == nullptr) {
      LogError(L"SHGetKnownFolderPath(FOLDERID_PublicDesktop) failed: " +
               std::format(kWindowsStatusFormat, static_cast<std::uint32_t>(result)));
      return std::filesystem::path{};
    }

    const auto result_path = std::filesystem::path(public_desktop);

    CoTaskMemFree(public_desktop);

    return result_path;
  }();

  return path;
}

std::expected<void, std::error_code>
CreateCrashTriggerFile(const std::filesystem::path& directory) {
  if (directory.empty()) {
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }

  const auto path = directory / kCrashTriggerFile;

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
}

void DeleteCrashTriggerFile(const std::filesystem::path& directory) {
  if (directory.empty()) {
    return;
  }

  std::error_code error;

  if (std::filesystem::remove(directory / kCrashTriggerFile, error)) {
    LogInfo(L"Deleted crash trigger file.");
  } else if (error) {
    LogError(L"Could not delete crash trigger file. Error code: " + std::to_wstring(error.value()));
  }
}

std::expected<bool, std::error_code> CrashTriggerExists(const std::filesystem::path& directory) {
  if (directory.empty()) {
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }

  std::error_code error;

  const bool exists = std::filesystem::exists(directory / kCrashTriggerFile, error);

  if (error) {
    return std::unexpected(error);
  }

  return exists;
}

std::expected<void, HRESULT> CreateDesktopShortcut(const std::filesystem::path& desktop,
                                                   const std::filesystem::path& target) {
  if (desktop.empty() || target.empty()) {
    return std::unexpected(E_INVALIDARG);
  }

  const HRESULT init_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  if (init_result != S_OK && init_result != S_FALSE) {
    return std::unexpected(init_result);
  }

  const auto shortcut_path = desktop / kDesktopShortcut;

  IShellLinkW* shell_link = nullptr;

  HRESULT result =
      CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell_link));

  if (SUCCEEDED(result)) {
    result = shell_link->SetPath(target.c_str());
  }

  if (SUCCEEDED(result)) {
    result = shell_link->SetDescription(L"TestService data directory");
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

  CoUninitialize();

  if (FAILED(result)) {
    return std::unexpected(result);
  }

  LogInfo(L"Created desktop shortcut: " + shortcut_path.wstring());

  return {};
}

LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exception) {
  EnsureProgramDataDirectory();

  HANDLE file = CreateFileW((ProgramDataDirectory() / kDumpFile).c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

  if (file == INVALID_HANDLE_VALUE) {
    OutputDebugStringW((L"Could not create crash dump file: " +
                        std::format(kWindowsStatusFormat, GetLastError()) + L"\n")
                           .c_str());

    return EXCEPTION_EXECUTE_HANDLER;
  }

  MINIDUMP_EXCEPTION_INFORMATION info{};

  info.ThreadId = GetCurrentThreadId();
  info.ExceptionPointers = exception;
  info.ClientPointers = FALSE;

  const BOOL dump_written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                              MiniDumpWithFullMemory, &info, nullptr, nullptr);

  if (dump_written == FALSE) {
    OutputDebugStringW(
        (L"MiniDumpWriteDump failed: " + std::format(kWindowsStatusFormat, GetLastError()) + L"\n")
            .c_str());
  }

  CloseHandle(file);

  return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void Crash() {
  LogError(L"Service crash triggered.");

  volatile int* invalid_address = reinterpret_cast<volatile int*>(0x1);

  *invalid_address = 42;  // NOLINT(cppcoreguidelines-avoid-magic-numbers)

  std::abort();
}

void ReportStatus(DWORD state, DWORD win32_exit_code = NO_ERROR, DWORD wait_hint = 0) {
  SERVICE_STATUS status{};

  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = state;
  status.dwWin32ExitCode = win32_exit_code;
  status.dwWaitHint = wait_hint;

  if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
    status.dwControlsAccepted = 0;
  } else {
    status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  }

  g_service_state.store(state, std::memory_order_release);

  if (g_service_status_handle != nullptr &&
      SetServiceStatus(g_service_status_handle, &status) == FALSE) {
    const DWORD error = GetLastError();

    OutputDebugStringW(
        (L"SetServiceStatus failed: " + std::format(kWindowsStatusFormat, error) + L"\n").c_str());
  }
}

void WorkerThread(const std::filesystem::path& directory) {
  while (true) {
    const DWORD result = WaitForSingleObject(
        g_stop_event,
        static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(kWorkerInterval).count()));

    switch (result) {
      case WAIT_OBJECT_0:
        return;

      case WAIT_TIMEOUT:
        break;

      case WAIT_FAILED:
        LogError(L"WaitForSingleObject failed: " +
                 std::format(kWindowsStatusFormat, GetLastError()));
        return;

      default:
        LogError(L"Unexpected wait result: " + std::format(kWindowsStatusFormat, result));
        return;
    }

    const auto trigger_exists = CrashTriggerExists(directory);

    if (!trigger_exists) {
      const auto& error = trigger_exists.error();

      LogError(L"Could not check crash trigger file. Error code: " +
               std::to_wstring(error.value()));
    } else if (!*trigger_exists) {
      LogInfo(L"Crash trigger deleted.");
      Crash();
    }

    LogInfo(L"Service alive.");
  }
}

DWORD WINAPI ServiceControlHandler(DWORD control, [[maybe_unused]] DWORD event_type,
                                   [[maybe_unused]] void* event_data,
                                   [[maybe_unused]] void* context) {
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

    LogError(L"SetEvent failed: " + std::format(kWindowsStatusFormat, error));

    return error;
  }

  ReportStatus(SERVICE_STOP_PENDING, NO_ERROR,
               static_cast<DWORD>(
                   std::chrono::duration_cast<std::chrono::milliseconds>(kStopDelay).count()));

  return NO_ERROR;
}

void WINAPI ServiceMain([[maybe_unused]] DWORD argc, [[maybe_unused]] LPWSTR* argv) {
  g_service_status_handle =
      RegisterServiceCtrlHandlerExW(service_name, ServiceControlHandler, nullptr);

  if (g_service_status_handle == nullptr) {
    // No status handle means SERVICE_STOPPED can never be reported to SCM,
    // so fall back to the debugger rather than failing silently.
    OutputDebugStringW((L"RegisterServiceCtrlHandlerExW failed: " +
                        std::format(kWindowsStatusFormat, GetLastError()) + L"\n")
                           .c_str());

    return;
  }

  LogInfo(std::format(L"Service starting - simulating {} second startup.", kStartDelay.count()));

  ReportStatus(SERVICE_START_PENDING, NO_ERROR,
               static_cast<DWORD>(
                   std::chrono::duration_cast<std::chrono::milliseconds>(kStartDelay).count()));

  g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  if (g_stop_event == nullptr) {
    const DWORD error = GetLastError();

    LogError(L"CreateEventW failed: " + std::format(kWindowsStatusFormat, error));

    ReportStatus(SERVICE_STOPPED, error);

    return;
  }

  std::this_thread::sleep_for(kStartDelay);

  LogInfo(L"Service startup delay completed.");

  if (ProgramDataDirectory().empty()) {
    LogError(L"Could not determine ProgramData service directory.");

    ReportStatus(SERVICE_STOPPED, ERROR_PATH_NOT_FOUND);

    CloseHandle(g_stop_event);
    g_stop_event = nullptr;

    return;
  }

  EnsureProgramDataDirectory();

  const auto trigger_result = CreateCrashTriggerFile(ProgramDataDirectory());

  if (!trigger_result) {
    const auto& error = trigger_result.error();

    LogError(L"Service startup failed. Error code: " + std::to_wstring(error.value()));

    ReportStatus(SERVICE_STOPPED, ERROR_WRITE_FAULT);

    CloseHandle(g_stop_event);
    g_stop_event = nullptr;

    return;
  }

  if (PublicDesktopDirectory().empty()) {
    LogError(
        L"Could not determine Public Desktop. "
        L"Continuing without desktop shortcut.");
  } else {
    const auto shortcut_result =
        CreateDesktopShortcut(PublicDesktopDirectory(), ProgramDataDirectory());

    if (!shortcut_result) {
      LogError(
          L"Could not create desktop shortcut. HRESULT: " +
          std::format(kWindowsStatusFormat, static_cast<std::uint32_t>(shortcut_result.error())));
    }
  }

  std::jthread worker_thread;

  try {
    worker_thread = std::jthread(WorkerThread, std::cref(ProgramDataDirectory()));
  } catch (const std::system_error& error) {
    LogError(L"Failed to create worker thread. Error code: " +
             std::to_wstring(error.code().value()));

    if (SetEvent(g_stop_event) == FALSE) {
      LogError(L"SetEvent failed: " + std::format(kWindowsStatusFormat, GetLastError()));
    }

    CloseHandle(g_stop_event);
    g_stop_event = nullptr;

    DeleteCrashTriggerFile(ProgramDataDirectory());

    ReportStatus(SERVICE_STOPPED, ERROR_NOT_ENOUGH_MEMORY);
    return;
  }

  ReportStatus(SERVICE_RUNNING);

  const DWORD wait_result = WaitForSingleObject(g_stop_event, INFINITE);

  if (wait_result == WAIT_FAILED) {
    const DWORD error = GetLastError();

    LogError(L"WaitForSingleObject failed: " + std::format(kWindowsStatusFormat, error));

    worker_thread.join();

    CloseHandle(g_stop_event);
    g_stop_event = nullptr;

    ReportStatus(SERVICE_STOPPED, error);
    return;
  }

  if (wait_result != WAIT_OBJECT_0) {
    LogError(L"Unexpected wait result: " + std::format(kWindowsStatusFormat, wait_result));

    worker_thread.join();

    CloseHandle(g_stop_event);
    g_stop_event = nullptr;

    ReportStatus(SERVICE_STOPPED, ERROR_INVALID_DATA);
    return;
  }

  LogInfo(std::format(L"Service stopping - simulating {} second shutdown.", kStopDelay.count()));

  // Wait for the worker to stop before deleting its trigger file and
  // completing the service shutdown.
  worker_thread.join();

  DeleteCrashTriggerFile(ProgramDataDirectory());

  std::this_thread::sleep_for(kStopDelay);

  LogInfo(L"Service shutdown delay completed.");

  CloseHandle(g_stop_event);
  // Do not reset g_stop_event here. The service is now shutting down
  // and will not process further control requests.

  ReportStatus(SERVICE_STOPPED);
}

}  // namespace

int wmain() {  // NOLINT(misc-use-internal-linkage,readability-identifier-naming)
  SetUnhandledExceptionFilter(UnhandledExceptionHandler);

  SERVICE_TABLE_ENTRYW service_table[] = {
      {
          const_cast<LPWSTR>(service_name),
          ServiceMain,
      },
      {
          nullptr,
          nullptr,
      },
  };

  if (StartServiceCtrlDispatcherW(service_table) == FALSE) {
    return static_cast<int>(GetLastError());
  }

  return 0;
}

// NOLINTEND(misc-include-cleaner,llvm-include-order)