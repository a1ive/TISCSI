// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/ldm/DynamicVolumes.h"

#include "Core/disk/DeviceDisk.h"
#include "Core/disk/File.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <mutex>
#include <set>

namespace tiscsi::ldm {
namespace {

constexpr uint32_t kMaxDiskIndex = 128;

// Dynamic volumes this process serves read-write (by GUID).
std::mutex g_openVolumesMutex;
std::set<std::string> g_openVolumes;

class VolumeLease {
public:
    explicit VolumeLease(std::string guid) : m_guid(std::move(guid)) {}
    ~VolumeLease()
    {
        std::lock_guard lock(g_openVolumesMutex);
        g_openVolumes.erase(m_guid);
    }
    VolumeLease(const VolumeLease&) = delete;
    VolumeLease& operator=(const VolumeLease&) = delete;

private:
    std::string m_guid;
};

std::shared_ptr<VolumeLease> AcquireLease(const Guid& guid)
{
    std::lock_guard lock(g_openVolumesMutex);
    if (!g_openVolumes.insert(guid.ToString()).second) {
        return nullptr;
    }
    return std::make_shared<VolumeLease>(guid.ToString());
}

// A member image shared by the volumes of this process that use it (an image
// opened read-write is exclusive, so two volumes on the same images could not
// otherwise both be served). Calls are serialized: volumes in different
// targets run on different threads.
class SharedImage final : public disk::Disk {
public:
    explicit SharedImage(std::unique_ptr<disk::Disk> image) : m_image(std::move(image)) {}

    uint32_t BytesPerSector() const override { return m_image->BytesPerSector(); }
    uint64_t Size() const override { return m_image->Size(); }
    bool IsReadOnly() const override { return m_image->IsReadOnly(); }
    std::wstring Description() const override { return m_image->Description(); }

    disk::IoResult ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out) override
    {
        std::lock_guard lock(m_mutex);
        return m_image->ReadSectors(lba, count, out);
    }

    disk::IoResult WriteSectors(uint64_t lba, std::span<const uint8_t> data) override
    {
        std::lock_guard lock(m_mutex);
        return m_image->WriteSectors(lba, data);
    }

    disk::IoResult Flush() override
    {
        std::lock_guard lock(m_mutex);
        return m_image->Flush();
    }

private:
    std::mutex m_mutex;
    std::unique_ptr<disk::Disk> m_image;
};

std::mutex g_imagesMutex;
std::map<std::wstring, std::weak_ptr<SharedImage>> g_images; // by lowercase full path

std::shared_ptr<disk::Disk> OpenSharedImage(const std::wstring& path, bool readOnly, disk::OpenError& error)
{
    std::wstring key = disk::FullPath(path);
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    std::lock_guard lock(g_imagesMutex);
    if (std::shared_ptr<SharedImage> image = g_images[key].lock()) {
        if (readOnly || !image->IsReadOnly()) {
            return image;
        }
        // Open read-only for another volume: cannot be reopened for writing.
        error.Set(ERROR_SHARING_VIOLATION, path);
        return nullptr;
    }
    std::unique_ptr<disk::Disk> image = disk::OpenDiskImage(path, readOnly, error);
    if (!image) {
        return nullptr;
    }
    auto shared = std::make_shared<SharedImage>(std::move(image));
    g_images[key] = shared;
    return shared;
}

struct Member {
    std::shared_ptr<disk::Disk> disk;
    std::wstring label; // disk number or image path
    int physicalIndex = -1;
    std::wstring imagePath;
};

// The member disks and everything assembled from them.
struct Assembly {
    std::vector<Member> members;
    std::vector<DiskGroup> groups;
    std::vector<std::unique_ptr<DynamicVolume>> volumes;

    const Member* MemberOf(const Guid& diskGuid) const
    {
        for (const DiskGroup& group : groups) {
            if (const DynamicDisk* disk = group.FindDisk(diskGuid)) {
                for (const Member& member : members) {
                    if (member.disk == disk->disk) {
                        return &member;
                    }
                }
            }
        }
        return nullptr;
    }
};

std::wstring Widen(const std::string& ascii) { return std::wstring(ascii.begin(), ascii.end()); }

std::wstring WindowsVolumeName(const Guid& guid) { return L"\\\\?\\Volume{" + Widen(guid.ToString()) + L"}\\"; }

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) { return towlower(x) == towlower(y); });
}

std::vector<std::wstring> WindowsVolumeNames()
{
    std::vector<std::wstring> names;
    wchar_t name[MAX_PATH] = {};
    HANDLE find = FindFirstVolumeW(name, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE) {
        return names;
    }
    do {
        names.emplace_back(name);
    } while (FindNextVolumeW(find, name, MAX_PATH));
    FindVolumeClose(find);
    return names;
}

std::wstring FindWindowsVolume(const Guid& guid, const std::vector<std::wstring>& windowsVolumes)
{
    const std::wstring name = WindowsVolumeName(guid);
    for (const std::wstring& windowsVolume : windowsVolumes) {
        if (EqualsIgnoreCase(windowsVolume, name)) {
            return windowsVolume;
        }
    }
    return {};
}

// "{guid}", "guid" or "\\?\Volume{guid}\".
std::optional<Guid> ParseVolumeGuid(std::wstring text)
{
    const std::wstring prefix = L"\\\\?\\Volume";
    if (text.size() > prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix)) {
        text.erase(0, prefix.size());
        if (!text.empty() && text.back() == L'\\') {
            text.pop_back();
        }
    }
    std::string ascii;
    for (wchar_t c : text) {
        if (c > 0x7F) {
            return std::nullopt;
        }
        ascii += static_cast<char>(c);
    }
    return Guid::Parse(ascii);
}

std::wstring BaseName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Opens the member disks. With `indices` / `paths` set, only those; otherwise
// every image, or every physical disk that can be opened.
bool OpenMembers(const std::vector<std::wstring>& images, bool readOnly, const std::vector<int>* indices,
                 const std::vector<std::wstring>* paths, std::vector<Member>& members, disk::OpenError& error)
{
    if (!images.empty()) {
        for (const std::wstring& path : images) {
            if (paths && std::find(paths->begin(), paths->end(), path) == paths->end()) {
                continue;
            }
            std::shared_ptr<disk::Disk> image = OpenSharedImage(path, readOnly, error);
            if (!image) {
                return false;
            }
            members.push_back({std::move(image), BaseName(path), -1, path});
        }
        return true;
    }

    bool accessDenied = false;
    for (uint32_t index = 0; index < kMaxDiskIndex; ++index) {
        if (indices && std::find(indices->begin(), indices->end(), static_cast<int>(index)) == indices->end()) {
            continue;
        }
        DWORD code = ERROR_SUCCESS;
        std::unique_ptr<disk::RawPhysicalDisk> device = disk::RawPhysicalDisk::Open(index, readOnly, code);
        if (!device) {
            if (indices) {
                // A member that was there a moment ago.
                error.Set(code, disk::PhysicalDrivePath(index),
                          code == ERROR_WRITE_PROTECT ? disk::OpenError::Problem::WriteProtected
                                                      : disk::OpenError::Problem::None);
                return false;
            }
            accessDenied |= code == ERROR_ACCESS_DENIED;
            continue; // no such disk, or no media
        }
        members.push_back({std::move(device), std::to_wstring(index), static_cast<int>(index), {}});
    }
    if (members.empty() && accessDenied) {
        error.Set(ERROR_ACCESS_DENIED, disk::PhysicalDrivePath(0));
        return false;
    }
    return true;
}

void Assemble(Assembly& assembly)
{
    std::vector<DynamicDisk> dynamicDisks;
    for (const Member& member : assembly.members) {
        if (std::optional<DynamicDisk> dynamicDisk = ReadDynamicDisk(member.disk)) {
            dynamicDisks.push_back(std::move(*dynamicDisk));
        }
    }
    assembly.groups = ReadDiskGroups(dynamicDisks);
    for (const DiskGroup& group : assembly.groups) {
        for (std::unique_ptr<DynamicVolume>& volume : BuildVolumes(group)) {
            assembly.volumes.push_back(std::move(volume));
        }
    }
}

bool Load(const std::vector<std::wstring>& images, bool readOnly, const std::vector<int>* indices,
          const std::vector<std::wstring>* paths, Assembly& assembly, disk::OpenError& error)
{
    if (!OpenMembers(images, readOnly, indices, paths, assembly.members, error)) {
        return false;
    }
    Assemble(assembly);
    return true;
}

} // namespace

DWORD ListDynamicVolumes(const std::vector<std::wstring>& images, std::vector<DynamicVolumeInfo>& volumes,
                         std::wstring& failedPath)
{
    volumes.clear();
    Assembly assembly;
    disk::OpenError error;
    if (!Load(images, true, nullptr, nullptr, assembly, error)) {
        failedPath = error.path;
        return error.code;
    }
    const std::vector<std::wstring> windowsVolumes = images.empty() ? WindowsVolumeNames() : std::vector<std::wstring>();
    for (const std::unique_ptr<DynamicVolume>& volume : assembly.volumes) {
        DynamicVolumeInfo info;
        info.guid = volume->VolumeGuid();
        info.name = volume->Name();
        info.diskGroupName = volume->DiskGroupName();
        info.layout = volume->Layout();
        info.health = volume->Health();
        info.size = volume->Size();
        for (const Guid& diskGuid : volume->MemberDisks()) {
            if (const Member* member = assembly.MemberOf(diskGuid)) {
                info.disks.push_back(member->label);
            }
        }
        info.windowsVolumeName = FindWindowsVolume(info.guid, windowsVolumes);
        volumes.push_back(std::move(info));
    }
    return ERROR_SUCCESS;
}

std::unique_ptr<disk::Disk> OpenDynamicVolume(const std::wstring& id, bool readOnly,
                                              const std::vector<std::wstring>& images, disk::OpenError& error)
{
    std::optional<Guid> guid = ParseVolumeGuid(id);
    if (!guid && images.empty() && id.find_first_of(L":\\") != std::wstring::npos) {
        // A drive letter or mount point of a volume Windows has.
        if (const std::optional<std::wstring> volumeName = disk::ResolveVolumeName(id)) {
            guid = ParseVolumeGuid(*volumeName);
        }
    }

    Assembly assembly;
    if (!Load(images, true, nullptr, nullptr, assembly, error)) {
        return nullptr;
    }
    std::vector<DynamicVolume*> matches;
    for (const std::unique_ptr<DynamicVolume>& volume : assembly.volumes) {
        const std::wstring name = Widen(volume->Name());
        if (guid ? volume->VolumeGuid() == *guid : EqualsIgnoreCase(name, id)) {
            matches.push_back(volume.get());
        }
    }
    if (matches.size() != 1) {
        error.Set(ERROR_FILE_NOT_FOUND, id,
                  matches.empty() ? disk::OpenError::Problem::NoSuchVolume : disk::OpenError::Problem::AmbiguousVolume);
        return nullptr;
    }
    DynamicVolume* volume = matches.front();
    const Guid volumeGuid = volume->VolumeGuid();

    if (images.empty()) {
        const std::wstring windowsVolume = FindWindowsVolume(volumeGuid, WindowsVolumeNames());
        if (!windowsVolume.empty()) {
            assembly = {}; // close the raw disk handles first
            return disk::VolumeDisk::Open(windowsVolume, readOnly, error);
        }
    }
    const VolumeHealth health = volume->Health();
    if (health == VolumeHealth::Failed) {
        error.Set(ERROR_FILE_NOT_FOUND, id, disk::OpenError::Problem::VolumeFailed);
        return nullptr;
    }
    if (readOnly) {
        for (std::unique_ptr<DynamicVolume>& candidate : assembly.volumes) {
            if (candidate.get() == volume) {
                candidate->MakeReadOnly(); // a shared member image may be writable
                return std::move(candidate); // keeps its member disks open
            }
        }
    }
    if (health != VolumeHealth::Healthy) {
        error.Set(ERROR_NOT_SUPPORTED, id, disk::OpenError::Problem::DegradedWrite);
        return nullptr;
    }
    std::shared_ptr<VolumeLease> lease = AcquireLease(volumeGuid);
    if (!lease) {
        error.Set(ERROR_SHARING_VIOLATION, id, disk::OpenError::Problem::AlreadyInUse);
        return nullptr;
    }

    // Reopen only the member disks, now for writing.
    std::vector<int> indices;
    std::vector<std::wstring> paths;
    for (const Guid& diskGuid : volume->MemberDisks()) {
        if (const Member* member = assembly.MemberOf(diskGuid)) {
            if (member->physicalIndex >= 0) {
                indices.push_back(member->physicalIndex);
            } else {
                paths.push_back(member->imagePath);
            }
        }
    }
    assembly = {};
    Assembly writable;
    if (!Load(images, false, &indices, &paths, writable, error)) {
        return nullptr;
    }
    for (std::unique_ptr<DynamicVolume>& candidate : writable.volumes) {
        if (candidate->VolumeGuid() == volumeGuid && candidate->Health() == VolumeHealth::Healthy) {
            candidate->SetLease(std::move(lease));
            return std::move(candidate);
        }
    }
    error.Set(ERROR_FILE_NOT_FOUND, id, disk::OpenError::Problem::NoSuchVolume);
    return nullptr;
}

} // namespace tiscsi::ldm
