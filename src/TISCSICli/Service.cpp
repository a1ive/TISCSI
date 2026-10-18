// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Service.h"

#include "Common/I18n.h"
#include "Console.h"
#include "Core/disk/Device.h"
#include "resource.h"

#include <winsvc.h>

#include <format>

namespace tiscsi::cli {
namespace {

constexpr wchar_t kDisplayName[] = L"TISCSI iSCSI Target";
// SERVICE_CONFIG_FAILURE_ACTIONS_FLAG (Vista+): also restart after the
// service stops with an error, not only after a crash.
constexpr DWORD kConfigFailureActionsFlag = 4;
struct FailureActionsFlag {
    BOOL failureActionsOnNonCrashFailures;
};

class ServiceHandle {
public:
    explicit ServiceHandle(SC_HANDLE handle = nullptr) : m_handle(handle) {}
    ~ServiceHandle()
    {
        if (m_handle) {
            CloseServiceHandle(m_handle);
        }
    }
    ServiceHandle(const ServiceHandle&) = delete;
    ServiceHandle& operator=(const ServiceHandle&) = delete;
    SC_HANDLE get() const { return m_handle; }
    explicit operator bool() const { return m_handle != nullptr; }
    void reset(SC_HANDLE handle)
    {
        if (m_handle) {
            CloseServiceHandle(m_handle);
        }
        m_handle = handle;
    }

private:
    SC_HANDLE m_handle;
};

void PrintServiceError(UINT id, const std::wstring& name, DWORD error)
{
    PrintError(i18n::Format(id, name, ErrorMessage(error)));
    if (error == ERROR_ACCESS_DENIED && !disk::IsProcessElevated()) {
        PrintError(i18n::Str(IDS_CLI_HINT_ADMIN_SERVICE));
    }
}

std::wstring ExecutablePath()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

// Polls while the service is in `state`, for at most `timeoutMs`.
SERVICE_STATUS WaitWhile(SC_HANDLE service, DWORD state, DWORD timeoutMs)
{
    SERVICE_STATUS status = {};
    const DWORD start = GetTickCount();
    while (QueryServiceStatus(service, &status) && status.dwCurrentState == state &&
           GetTickCount() - start < timeoutMs) {
        Sleep(250);
    }
    return status;
}

// Stops the service if it runs; false if it does not stop within a minute.
bool StopService(SC_HANDLE service)
{
    SERVICE_STATUS status = {};
    if (!QueryServiceStatus(service, &status)) {
        return false;
    }
    if (status.dwCurrentState != SERVICE_STOPPED && status.dwCurrentState != SERVICE_STOP_PENDING) {
        ControlService(service, SERVICE_CONTROL_STOP, &status);
    }
    const DWORD start = GetTickCount();
    while (QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED &&
           GetTickCount() - start < 60000) {
        Sleep(250);
    }
    return status.dwCurrentState == SERVICE_STOPPED;
}

// ---- Running under the service controller --------------------------------------

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
std::function<int()> g_run;
HANDLE g_stopEvent = nullptr;
int g_result = 0;

void ReportStatus(DWORD state, int result = 0, DWORD waitHint = 0)
{
    static DWORD checkPoint = 1;
    SERVICE_STATUS status = {};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    if (result != 0) {
        status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        status.dwServiceSpecificExitCode = static_cast<DWORD>(result);
    }
    status.dwWaitHint = waitHint;
    status.dwCheckPoint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : checkPoint++;
    SetServiceStatus(g_statusHandle, &status);
}

DWORD WINAPI ControlHandler(DWORD control, DWORD, LPVOID, LPVOID)
{
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ReportStatus(SERVICE_STOP_PENDING, 0, 30000);
        SetEvent(g_stopEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    // An own-process service may register under any name.
    g_statusHandle = RegisterServiceCtrlHandlerExW(kDefaultServiceName, ControlHandler, nullptr);
    if (!g_statusHandle) {
        return;
    }
    ReportStatus(SERVICE_RUNNING);
    g_result = g_run();
    ReportStatus(SERVICE_STOPPED, g_result);
}

} // namespace

bool IsValidServiceName(const std::wstring& name)
{
    return !name.empty() && name.size() <= 256 && name.find_first_of(L"/\\") == std::wstring::npos;
}

int InstallService(const std::wstring& name, const std::wstring& configFile, const std::wstring& logPath, bool waits)
{
    const std::wstring command = std::format(L"\"{}\" --service --config \"{}\"", ExecutablePath(), configFile);
    const std::wstring displayName =
        name == kDefaultServiceName ? std::wstring(kDisplayName) : std::format(L"{} ({})", kDisplayName, name);
    static constexpr wchar_t kDependencies[] = L"Tcpip\0";

    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!manager) {
        PrintServiceError(IDS_CLI_ERR_SERVICE_INSTALL, name, GetLastError());
        return 2;
    }
    bool updated = false;
    ServiceHandle service(OpenServiceW(manager.get(), name.c_str(), SERVICE_ALL_ACCESS));
    if (service) {
        // A running service keeps its old configuration until restarted.
        if (!StopService(service.get())) {
            PrintServiceError(IDS_CLI_ERR_SERVICE_INSTALL, name, ERROR_SERVICE_REQUEST_TIMEOUT);
            return 2;
        }
        if (!ChangeServiceConfigW(service.get(), SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                  command.c_str(), nullptr, nullptr, kDependencies, nullptr, nullptr,
                                  displayName.c_str())) {
            PrintServiceError(IDS_CLI_ERR_SERVICE_INSTALL, name, GetLastError());
            return 2;
        }
        updated = true;
    } else {
        service.reset(CreateServiceW(manager.get(), name.c_str(), displayName.c_str(), SERVICE_ALL_ACCESS,
                                     SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                     command.c_str(), nullptr, nullptr, kDependencies, nullptr, nullptr));
        if (!service) {
            PrintServiceError(IDS_CLI_ERR_SERVICE_INSTALL, name, GetLastError());
            return 2;
        }
    }

    std::wstring description = i18n::Format(IDS_CLI_SERVICE_DESCRIPTION, configFile);
    SERVICE_DESCRIPTIONW serviceDescription = {description.data()};
    ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, &serviceDescription);
    // Devices can come later than the wait allows: try again a minute later,
    // up to three times a day.
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 60000}, {SC_ACTION_RESTART, 60000}, {SC_ACTION_RESTART, 60000}};
    SERVICE_FAILURE_ACTIONSW failureActions = {24 * 60 * 60, nullptr, nullptr, 3, actions};
    ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS, &failureActions);
    FailureActionsFlag flag = {TRUE};
    ChangeServiceConfig2W(service.get(), kConfigFailureActionsFlag, &flag); // fails on XP; crashes still restart

    PrintLine(i18n::Format(updated ? IDS_CLI_SERVICE_UPDATED : IDS_CLI_SERVICE_INSTALLED, name, configFile));
    if (!waits) {
        PrintError(i18n::Str(IDS_CLI_WARN_NO_WAIT));
    }
    if (!StartServiceW(service.get(), 0, nullptr)) {
        PrintServiceError(IDS_CLI_ERR_SERVICE_START, name, GetLastError());
        return 2;
    }
    // A configuration that cannot be served stops the service within moments.
    WaitWhile(service.get(), SERVICE_START_PENDING, 10000);
    const SERVICE_STATUS status = WaitWhile(service.get(), SERVICE_RUNNING, 3000);
    if (status.dwCurrentState != SERVICE_RUNNING) {
        const DWORD code = status.dwWin32ExitCode == ERROR_SERVICE_SPECIFIC_ERROR ? status.dwServiceSpecificExitCode
                                                                                  : status.dwWin32ExitCode;
        PrintError(i18n::Format(IDS_CLI_ERR_SERVICE_STOPPED, name, code, logPath));
        return 2;
    }
    PrintLine(i18n::Format(IDS_CLI_SERVICE_RUNNING, logPath));
    return 0;
}

int UninstallService(const std::wstring& name)
{
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) {
        PrintServiceError(IDS_CLI_ERR_SERVICE_REMOVE, name, GetLastError());
        return 2;
    }
    ServiceHandle service(OpenServiceW(manager.get(), name.c_str(), DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!service) {
        PrintServiceError(IDS_CLI_ERR_SERVICE_REMOVE, name, GetLastError());
        return 2;
    }
    if (!StopService(service.get())) {
        PrintServiceError(IDS_CLI_ERR_SERVICE_REMOVE, name, ERROR_SERVICE_REQUEST_TIMEOUT);
        return 2;
    }
    if (!DeleteService(service.get())) {
        PrintServiceError(IDS_CLI_ERR_SERVICE_REMOVE, name, GetLastError());
        return 2;
    }
    PrintLine(i18n::Format(IDS_CLI_SERVICE_REMOVED, name));
    return 0;
}

DWORD RunAsService(std::function<int()> run, HANDLE stopEvent)
{
    g_run = std::move(run);
    g_stopEvent = stopEvent;
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kDefaultServiceName), ServiceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        return GetLastError();
    }
    return static_cast<DWORD>(g_result);
}

} // namespace tiscsi::cli
