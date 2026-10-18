// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// TISCSI.exe: the graphical front end (iSCSI Console's ISCSIConsole project).

#include "App.h"
#include "AppSettings.h"
#include "Common/I18n.h"
#include "Common/LogFile.h"
#include "Common/Text.h"
#include "MainDialog.h"
#include "UiUtil.h"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <format>
#include <memory>
#include <string>
#include <string_view>

using namespace tiscsi;

namespace {

struct CommandLine {
    std::wstring language;
    std::wstring logFile;
    std::wstring configFile;
    bool valid = true;
};

// "Log yyyy-MM-dd HH-mm.txt" next to the executable, like iSCSI Console's /log.
std::wstring DefaultLogPath()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    path.resize(path.find_last_of(L'\\') + 1);
    SYSTEMTIME now;
    GetLocalTime(&now);
    return path + std::format(L"Log {:04}-{:02}-{:02} {:02}-{:02}.txt", now.wYear, now.wMonth, now.wDay, now.wHour,
                              now.wMinute);
}

CommandLine ParseCommandLine()
{
    CommandLine result;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return result;
    }
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg.starts_with(L"--lang=")) {
            result.language = arg.substr(7);
        } else if (arg == L"--lang" && i + 1 < argc) {
            result.language = argv[++i];
        } else if (arg == L"/log") {
            result.logFile = DefaultLogPath();
        } else if (arg.starts_with(L"--log=")) {
            result.logFile = arg.substr(6);
        } else if (arg == L"--log" && i + 1 < argc) {
            result.logFile = argv[++i];
        } else if (arg.starts_with(L"--config=")) {
            result.configFile = arg.substr(9);
        } else if (arg == L"--config" && i + 1 < argc) {
            result.configFile = argv[++i];
        } else {
            result.valid = false;
        }
    }
    LocalFree(argv);
    return result;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    const CommandLine commandLine = ParseCommandLine();
    std::wstring language = commandLine.language.empty() ? settings::LoadLanguage() : commandLine.language;
    i18n::Initialize(instance, language);
    if (!commandLine.valid) {
        ui::ShowInfo(nullptr, i18n::Str(IDS_USAGE));
        return 1;
    }

    LogFile logFile;
    if (!commandLine.logFile.empty()) {
        if (const DWORD error = logFile.Open(commandLine.logFile)) {
            ui::ShowError(nullptr, i18n::Format(IDS_ERR_LOG_FILE, commandLine.logFile, text::ErrorMessage(error)));
        }
    }
    auto sink = std::make_shared<const util::LogSink>(
        [&logFile](util::Severity severity, std::string_view source, std::string_view message) {
            if (severity != util::Severity::Trace) {
                logFile.Write(severity, source, message);
            }
        });

    // Declared after logFile: the server stops before the log is closed.
    App app(sink);
    app.startupConfig = commandLine.configFile;
    for (;;) {
        MainDialog dialog(app, language);
        if (dialog.Run() != MainDialog::kRecreate) {
            break;
        }
    }
    return 0;
}
