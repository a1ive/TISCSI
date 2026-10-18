// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// TISCSICli: serves disk images and RAM disks over iSCSI from the command line.

#include "Common/I18n.h"
#include "Common/LogFile.h"
#include "Console.h"
#include "Core/config/ServerConfig.h"
#include "Core/disk/DeviceDisk.h"
#include "Core/disk/DeviceSelector.h"
#include "Core/disk/DiskImage.h"
#include "Core/disk/Vhd.h"
#include "Core/scsi/SptiTarget.h"
#include "Core/iscsi/IscsiServer.h"
#include "Core/ldm/DynamicVolumes.h"
#include "Options.h"
#include "resource.h"

#include <winsock2.h>

#include <algorithm>
#include <format>
#include <memory>

using namespace tiscsi;
using util::Severity;

namespace {

HANDLE g_stopEvent = nullptr;

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        SetEvent(g_stopEvent);
        return TRUE;
    default:
        return FALSE;
    }
}

// Writes log entries to stderr and optionally to a file (UTF-8).
class LogWriter {
public:
    explicit LogWriter(Severity level) : m_level(level) {}

    DWORD OpenFile(const std::wstring& path) { return m_file.Open(path); }

    void Write(Severity severity, std::string_view source, std::string_view message)
    {
        if (static_cast<int>(severity) > static_cast<int>(m_level)) {
            return;
        }
        cli::PrintError(cli::Utf8ToWide(LogFile::FormatLine(severity, source, message)));
        m_file.Write(severity, source, message);
    }

private:
    Severity m_level;
    LogFile m_file;
};

// Devices need administrator rights; say so instead of a bare "Access is denied".
void PrintAdminHint(DWORD error)
{
    if (error == ERROR_ACCESS_DENIED && !disk::IsProcessElevated()) {
        cli::PrintError(i18n::Str(IDS_CLI_HINT_ADMIN));
    }
}

void PrintOpenError(const disk::OpenError& error, bool device)
{
    switch (error.problem) {
    case disk::OpenError::Problem::DynamicDiskWrite:
        cli::PrintError(i18n::Format(IDS_CLI_ERR_DYNAMIC_DISK, error.path));
        return;
    case disk::OpenError::Problem::VolumeLocked:
        cli::PrintError(i18n::Format(IDS_CLI_ERR_VOLUME_LOCKED, error.path, cli::ErrorMessage(error.code)));
        PrintAdminHint(error.code);
        return;
    case disk::OpenError::Problem::AlreadyInUse:
        cli::PrintError(i18n::Format(IDS_CLI_ERR_ALREADY_SERVED, error.path));
        return;
    case disk::OpenError::Problem::WriteProtected:
        cli::PrintError(i18n::Format(IDS_CLI_ERR_WRITE_PROTECTED, error.path));
        return;
    case disk::OpenError::Problem::NoSuchVolume:
        cli::PrintError(i18n::Format(IDS_CLI_ERR_NO_SUCH_VOLUME, error.path));
        return;
    case disk::OpenError::Problem::AmbiguousVolume:
        cli::PrintError(i18n::Format(IDS_CLI_ERR_AMBIGUOUS_VOLUME, error.path));
        return;
    case disk::OpenError::Problem::VolumeFailed:
        cli::PrintError(i18n::Format(IDS_CLI_ERR_VOLUME_FAILED, error.path));
        return;
    case disk::OpenError::Problem::DegradedWrite:
        cli::PrintError(i18n::Format(IDS_CLI_ERR_DEGRADED_WRITE, error.path));
        return;
    case disk::OpenError::Problem::NoSuchDevice:
        cli::PrintError(i18n::Format(IDS_CLI_ERR_NO_SUCH_DEVICE, error.path));
        return;
    case disk::OpenError::Problem::AmbiguousDevice:
        cli::PrintError(i18n::Format(IDS_CLI_ERR_AMBIGUOUS_DEVICE, error.path));
        return;
    case disk::OpenError::Problem::None:
        // error.path may name a parent or extent file rather than the image itself.
        cli::PrintError(i18n::Format(device ? IDS_CLI_ERR_OPEN_DEVICE : IDS_CLI_ERR_OPEN_IMAGE, error.path,
                                     cli::ErrorMessage(error.code)));
        PrintAdminHint(error.code);
        return;
    }
}

UINT LayoutName(ldm::VolumeLayout layout)
{
    switch (layout) {
    case ldm::VolumeLayout::Simple:
        return IDS_CLI_LAYOUT_SIMPLE;
    case ldm::VolumeLayout::Spanned:
        return IDS_CLI_LAYOUT_SPANNED;
    case ldm::VolumeLayout::Striped:
        return IDS_CLI_LAYOUT_STRIPED;
    case ldm::VolumeLayout::Mirrored:
        return IDS_CLI_LAYOUT_MIRRORED;
    case ldm::VolumeLayout::Raid5:
        return IDS_CLI_LAYOUT_RAID5;
    }
    return IDS_CLI_LAYOUT_SIMPLE;
}

UINT HealthName(ldm::VolumeHealth health)
{
    switch (health) {
    case ldm::VolumeHealth::Healthy:
        return IDS_CLI_HEALTH_HEALTHY;
    case ldm::VolumeHealth::FailedRedundancy:
        return IDS_CLI_HEALTH_FAILED_REDUNDANCY;
    case ldm::VolumeHealth::Failed:
        return IDS_CLI_HEALTH_FAILED;
    }
    return IDS_CLI_HEALTH_FAILED;
}

// Dynamic volumes need raw disk access: without administrator rights (and
// without --ldm-disk images) the section is left out.
void ListDynamicVolumes(const std::vector<std::wstring>& images)
{
    std::vector<ldm::DynamicVolumeInfo> volumes;
    std::wstring failedPath;
    const DWORD error = ldm::ListDynamicVolumes(images, volumes, failedPath);
    if (error == ERROR_ACCESS_DENIED && images.empty()) {
        return;
    }
    cli::PrintLine(i18n::Str(IDS_CLI_LIST_DYNAMIC_VOLUMES));
    if (error != ERROR_SUCCESS) {
        cli::PrintError(i18n::Format(IDS_CLI_ERR_OPEN_DEVICE, failedPath, cli::ErrorMessage(error)));
        return;
    }
    for (const ldm::DynamicVolumeInfo& info : volumes) {
        std::wstring disks;
        for (const std::wstring& disk : info.disks) {
            disks += (disks.empty() ? L"" : L", ") + disk;
        }
        const std::wstring mounted = info.windowsVolumeName.empty()
                                         ? i18n::Str(IDS_CLI_LIST_NOT_MOUNTED)
                                         : i18n::Format(IDS_CLI_LIST_WINDOWS_VOLUME, info.windowsVolumeName);
        cli::PrintLine(i18n::Format(IDS_CLI_LIST_DYNAMIC_VOLUME, cli::Utf8ToWide(info.name),
                                    cli::Utf8ToWide("{" + info.guid.ToString() + "}"),
                                    cli::Utf8ToWide(info.diskGroupName), i18n::Str(LayoutName(info.layout)),
                                    i18n::Str(HealthName(info.health)), cli::FormatSize(info.size),
                                    disks.empty() ? L"-" : disks, mounted));
    }
}

template <typename Info>
std::string Model(const Info& info)
{
    return info.vendor.empty() || info.product.empty() ? info.vendor + info.product
                                                       : info.vendor + " " + info.product;
}

// The names to use in place of disk numbers, device paths and drive letters.
void PrintStableNames(const std::vector<std::wstring>& names)
{
    if (names.empty()) {
        return;
    }
    std::wstring text;
    for (const std::wstring& name : names) {
        const bool quote = name.find(L' ') != std::wstring::npos;
        text += (text.empty() ? L"" : L"  ") + (quote ? L"\"" + name + L"\"" : name);
    }
    cli::PrintLine(i18n::Format(IDS_CLI_LIST_STABLE_NAMES, text));
}

// A device given by a stable name also shows what it matched.
std::wstring LunDescription(const config::DiskSpec& spec, const disk::Disk& disk)
{
    switch (spec.kind) {
    case config::DiskSpec::Kind::Image:
        return spec.path;
    case config::DiskSpec::Kind::RamDisk:
        return i18n::Str(IDS_CLI_RAM_DISK);
    case config::DiskSpec::Kind::PhysicalDisk: {
        const uint32_t index = static_cast<const disk::PhysicalDisk&>(disk).Index();
        const std::wstring description = i18n::Format(IDS_CLI_PHYSICAL_DISK, index);
        return spec.path == std::to_wstring(index) ? description : std::format(L"{} ({})", description, spec.path);
    }
    case config::DiskSpec::Kind::Volume: {
        const std::wstring description = i18n::Format(IDS_CLI_VOLUME, spec.path);
        return disk::IsStableVolumeSelector(spec.path) ? std::format(L"{} ({})", description, disk.Description())
                                                       : description;
    }
    case config::DiskSpec::Kind::DynamicVolume:
        return i18n::Format(IDS_CLI_DYNAMIC_VOLUME, disk.Description());
    }
    return spec.path;
}

void ListDevices(const std::vector<std::wstring>& ldmImages)
{
    if (!ldmImages.empty()) {
        ListDynamicVolumes(ldmImages);
        return;
    }
    cli::PrintLine(i18n::Str(IDS_CLI_LIST_DISKS));
    const std::vector<disk::PhysicalDiskInfo> physicalDisks = disk::ListPhysicalDisks();
    for (const disk::PhysicalDiskInfo& info : physicalDisks) {
        cli::PrintLine(i18n::Format(IDS_CLI_LIST_DISK, info.index, cli::Utf8ToWide(Model(info)),
                                    cli::FormatSize(info.size),
                                    info.isDynamic ? i18n::Str(IDS_CLI_LIST_DYNAMIC) : std::wstring()));
        PrintStableNames(disk::StableDiskSelectors(info, physicalDisks));
    }
    cli::PrintLine(i18n::Str(IDS_CLI_LIST_VOLUMES));
    const std::vector<disk::VolumeInfo> volumes = disk::ListVolumes();
    for (const disk::VolumeInfo& info : volumes) {
        std::wstring mountPoints;
        for (const std::wstring& mountPoint : info.mountPoints) {
            mountPoints += (mountPoints.empty() ? L"" : L", ") + mountPoint;
        }
        std::wstring disks;
        for (uint32_t number : info.diskNumbers) {
            disks += (disks.empty() ? L"" : L", ") + std::to_wstring(number);
        }
        cli::PrintLine(i18n::Format(IDS_CLI_LIST_VOLUME, info.volumeName, mountPoints.empty() ? L"-" : mountPoints,
                                    info.fileSystem.empty() ? L"-" : info.fileSystem, info.label,
                                    cli::FormatSize(info.size), disks.empty() ? L"-" : disks));
        if (const std::optional<std::wstring> name = disk::StableVolumeSelector(info, physicalDisks, volumes)) {
            PrintStableNames({*name});
        }
    }
    ListDynamicVolumes({});
    cli::PrintLine(i18n::Str(IDS_CLI_LIST_SCSI));
    const std::vector<disk::ScsiDeviceInfo> scsiDevices = disk::ListScsiDevices();
    for (const disk::ScsiDeviceInfo& info : scsiDevices) {
        const std::string model = Model(info);
        cli::PrintLine(L"  " + info.path + (model.empty() ? L"" : L"  " + cli::Utf8ToWide(model)));
        PrintStableNames(disk::StableScsiSelectors(info, scsiDevices));
    }
    if (!disk::IsProcessElevated()) {
        cli::PrintLine(i18n::Str(IDS_CLI_HINT_ADMIN));
    }
}

bool CreateImage(const cli::Options& options)
{
    const std::wstring& path = options.createVhdPath;
    cli::PrintLine(i18n::Format(IDS_CLI_CREATING, path, cli::FormatSize(options.createSize),
                                i18n::Str(options.createDynamic ? IDS_CLI_VHD_DYNAMIC : IDS_CLI_VHD_FIXED)));
    DWORD error;
    if (options.createDynamic) {
        error = disk::CreateDynamicVhd(path, options.createSize);
    } else {
        int lastPercent = -1;
        error = disk::CreateFixedVhd(path, options.createSize, [&lastPercent](uint64_t done, uint64_t total) {
            const int percent = static_cast<int>(done * 100 / total);
            if (percent != lastPercent) {
                lastPercent = percent;
                cli::WriteErr(std::format(L"\r  {}%", percent));
            }
            return WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0; // Ctrl+C cancels
        });
        cli::WriteErr(L"\n");
    }
    if (error != ERROR_SUCCESS) {
        cli::PrintError(i18n::Format(IDS_CLI_ERR_CREATE, path, cli::ErrorMessage(error)));
        return false;
    }
    cli::PrintLine(i18n::Format(IDS_CLI_CREATED, path));
    return true;
}

// --save-config: devices are written by their stable names where they have one.
int SaveConfig(const cli::Options& options)
{
    config::ServerConfig saved = options.server;
    for (config::TargetSpec& target : saved.targets) {
        if (!target.sptiDevice.empty() && !config::UseStableSptiName(target.sptiDevice)) {
            cli::PrintError(i18n::Format(IDS_CLI_WARN_UNSTABLE, target.sptiDevice));
        }
        for (config::DiskSpec& disk : target.disks) {
            if (config::UseStableName(disk, saved.ldmImages)) {
                continue;
            }
            std::wstring description = disk.path;
            if (disk.kind == config::DiskSpec::Kind::PhysicalDisk) {
                description = i18n::Format(IDS_CLI_PHYSICAL_DISK, disk.path);
            } else if (disk.kind == config::DiskSpec::Kind::Volume) {
                description = i18n::Format(IDS_CLI_VOLUME, disk.path);
            } else if (disk.kind == config::DiskSpec::Kind::DynamicVolume) {
                description = i18n::Format(IDS_CLI_DYNAMIC_VOLUME, disk.path);
            }
            cli::PrintError(i18n::Format(IDS_CLI_WARN_UNSTABLE, description));
        }
    }
    if (const DWORD error = config::SaveConfigFile(options.saveConfigFile, saved)) {
        cli::PrintError(i18n::Format(IDS_CLI_ERR_CONFIG_SAVE, options.saveConfigFile, cli::ErrorMessage(error)));
        return 2;
    }
    cli::PrintLine(i18n::Format(IDS_CLI_CONFIG_SAVED, options.saveConfigFile));
    return 0;
}

struct LunInfo {
    std::wstring description;
    uint64_t size;
    bool readOnly;
};

struct OpenedTarget {
    size_t index; // in ServerConfig::targets
    std::shared_ptr<iscsi::IscsiTarget> target;
    std::vector<LunInfo> luns;
    bool passThrough;
};

enum class OpenResult { Opened, NotYet, Failed };

// Opens all of a target's disks or none. Without `report`, failures that may
// go away by themselves are not printed but returned as NotYet.
OpenResult OpenTarget(const config::TargetSpec& spec, const config::ServerConfig& server,
                      const std::shared_ptr<const util::LogSink>& sink, bool report, OpenedTarget& opened)
{
    std::vector<LunInfo> luns;
    if (!spec.sptiDevice.empty()) {
        std::wstring path;
        disk::OpenError error;
        if (!config::ResolveSptiDevice(spec.sptiDevice, path, error)) {
            if (!report && config::MayAppearLater(error)) {
                return OpenResult::NotYet;
            }
            PrintOpenError(error, true);
            return OpenResult::Failed;
        }
        DWORD code = 0;
        auto spti = scsi::SptiTarget::Open(path, util::Logger("SPTI Target", sink), code);
        if (!spti) {
            error.Set(code, path);
            if (!report && config::MayAppearLater(error)) {
                return OpenResult::NotYet;
            }
            cli::PrintError(i18n::Format(IDS_CLI_ERR_OPEN_SPTI, path, cli::ErrorMessage(code)));
            PrintAdminHint(code);
            return OpenResult::Failed;
        }
        const std::wstring device = path == spec.sptiDevice ? path : std::format(L"{} ({})", spec.sptiDevice, path);
        luns.push_back({i18n::Format(IDS_CLI_SPTI, device, spti->LunCount()), 0, false});
        opened.target = std::make_shared<iscsi::IscsiTarget>(spec.name, std::move(spti));
        opened.luns = std::move(luns);
        opened.passThrough = true;
        return OpenResult::Opened;
    }

    std::vector<std::unique_ptr<disk::Disk>> disks;
    for (const config::DiskSpec& diskSpec : spec.disks) {
        disk::OpenError error;
        std::unique_ptr<disk::Disk> disk = config::OpenDisk(diskSpec, server.ldmImages, error);
        if (!disk) {
            if (diskSpec.kind == config::DiskSpec::Kind::RamDisk) {
                cli::PrintError(i18n::Format(IDS_CLI_ERR_RAM_ALLOC, cli::FormatSize(diskSpec.size)));
                return OpenResult::Failed;
            }
            if (!report && config::MayAppearLater(error)) {
                return OpenResult::NotYet; // the disks opened so far are released
            }
            PrintOpenError(error, diskSpec.kind != config::DiskSpec::Kind::Image);
            return OpenResult::Failed;
        }
        luns.push_back({LunDescription(diskSpec, *disk), disk->Size(), disk->IsReadOnly()});
        disks.push_back(std::move(disk));
    }
    opened.target = std::make_shared<iscsi::IscsiTarget>(spec.name, std::move(disks),
                                                         util::Logger("Virtual SCSI Target", sink));
    opened.luns = std::move(luns);
    opened.passThrough = false;
    return OpenResult::Opened;
}

// Waits a second between attempts; false if Ctrl+C was pressed meanwhile.
bool WaitBeforeRetry() { return WaitForSingleObject(g_stopEvent, 1000) == WAIT_TIMEOUT; }

int Run(const cli::Options& options)
{
    if (!options.createVhdPath.empty()) {
        if (!CreateImage(options)) {
            return 2;
        }
        if (options.server.targets.empty()) {
            return 0;
        }
    }
    if (!options.saveConfigFile.empty()) {
        return SaveConfig(options);
    }

    LogWriter logWriter(options.server.verbosity);
    if (!options.server.logFile.empty()) {
        if (const DWORD error = logWriter.OpenFile(options.server.logFile)) {
            cli::PrintError(i18n::Format(IDS_CLI_ERR_LOG_FILE, options.server.logFile, cli::ErrorMessage(error)));
            return 2;
        }
    }
    auto sink = std::make_shared<const util::LogSink>(
        [&logWriter](Severity severity, std::string_view source, std::string_view message) {
            logWriter.Write(severity, source, message);
        });

    // Open every disk before starting, so a typo does not leave a half-started
    // server. Devices and files that are not there yet are waited for.
    const DWORD start = GetTickCount();
    const DWORD waitMs = options.server.waitSeconds * 1000;
    auto timeLeft = [&] { return GetTickCount() - start < waitMs; };
    std::vector<OpenedTarget> opened;
    std::vector<size_t> pending;
    for (size_t i = 0; i < options.server.targets.size(); ++i) {
        pending.push_back(i);
    }
    std::vector<bool> announced(options.server.targets.size(), false);
    while (!pending.empty()) {
        const bool last = !timeLeft();
        std::vector<size_t> stillPending;
        for (size_t index : pending) {
            const config::TargetSpec& spec = options.server.targets[index];
            OpenedTarget target{index, nullptr, {}, false};
            switch (OpenTarget(spec, options.server, sink, last, target)) {
            case OpenResult::Opened:
                opened.push_back(std::move(target));
                break;
            case OpenResult::NotYet:
                if (!announced[index]) {
                    announced[index] = true;
                    cli::PrintLine(i18n::Format(IDS_CLI_WAITING, cli::Utf8ToWide(spec.name), options.server.waitSeconds));
                }
                stillPending.push_back(index);
                break;
            case OpenResult::Failed:
                if (!options.server.skipUnavailable) {
                    return 2;
                }
                cli::PrintError(i18n::Format(IDS_CLI_TARGET_SKIPPED, cli::Utf8ToWide(spec.name)));
                break;
            }
        }
        pending = std::move(stillPending);
        if (!pending.empty() && !WaitBeforeRetry()) {
            cli::PrintLine(i18n::Str(IDS_CLI_STOPPED));
            return 2;
        }
    }
    if (opened.empty()) {
        cli::PrintError(i18n::Str(IDS_CLI_ERR_NOTHING_TO_SERVE));
        return 2;
    }
    std::sort(opened.begin(), opened.end(),
              [](const OpenedTarget& a, const OpenedTarget& b) { return a.index < b.index; });

    // Declared after `opened` so it is destroyed (and stopped) first.
    iscsi::IscsiServer server(util::Logger("iSCSI Server", sink));
    for (const OpenedTarget& target : opened) {
        server.AddTarget(target.target);
    }
    bool announcedAddress = false;
    for (;;) {
        const int error = server.Start(options.server.address, options.server.port);
        if (error == 0) {
            break;
        }
        // The address may come later (DHCP, a network adapter starting).
        if (error != WSAEADDRNOTAVAIL || !timeLeft()) {
            cli::PrintError(i18n::Format(IDS_CLI_ERR_START, cli::ErrorMessage(static_cast<unsigned long>(error))));
            return 2;
        }
        if (!announcedAddress) {
            announcedAddress = true;
            cli::PrintLine(i18n::Format(IDS_CLI_WAITING_ADDRESS, options.server.addressText, options.server.waitSeconds));
        }
        if (!WaitBeforeRetry()) {
            cli::PrintLine(i18n::Str(IDS_CLI_STOPPED));
            return 2;
        }
    }

    cli::PrintLine(i18n::Format(IDS_CLI_LISTENING, options.server.addressText, options.server.port));
    for (const OpenedTarget& target : opened) {
        cli::PrintLine(i18n::Format(IDS_CLI_TARGET, cli::Utf8ToWide(target.target->Name())));
        if (target.passThrough) {
            cli::PrintLine(target.luns.front().description);
            continue;
        }
        for (size_t lun = 0; lun < target.luns.size(); ++lun) {
            const LunInfo& info = target.luns[lun];
            cli::PrintLine(i18n::Format(IDS_CLI_LUN, lun, info.description, cli::FormatSize(info.size),
                                        info.readOnly ? i18n::Str(IDS_CLI_READ_ONLY) : std::wstring()));
        }
    }
    cli::PrintLine(i18n::Str(IDS_CLI_PRESS_CTRL_C));

    WaitForSingleObject(g_stopEvent, INFINITE);
    cli::PrintLine(i18n::Str(IDS_CLI_STOPPING));
    server.Stop();
    cli::PrintLine(i18n::Str(IDS_CLI_STOPPED));
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    i18n::Initialize(GetModuleHandleW(nullptr), cli::FindLanguageOption(argc, argv));

    cli::Options options;
    if (const auto error = cli::ParseOptions(argc, argv, options)) {
        cli::PrintError(*error);
        cli::PrintError(i18n::Str(IDS_CLI_ERR_HINT_HELP));
        return 1;
    }
    if (options.help) {
        cli::WriteOut(i18n::Str(IDS_CLI_USAGE));
        return 0;
    }
    if (options.list) {
        ListDevices(options.server.ldmImages);
        return 0;
    }

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    const int result = Run(options);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    CloseHandle(g_stopEvent);
    return result;
}
