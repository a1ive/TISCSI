// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive

#include "Core/disk/File.h"

#include <algorithm>
#include <utility>

namespace tiscsi::disk {
namespace {

constexpr size_t kMaxChunk = 64 * 1024 * 1024;

OVERLAPPED OffsetOverlapped(uint64_t offset)
{
    OVERLAPPED ov = {};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return ov;
}

} // namespace

std::unique_ptr<File> File::Open(const std::wstring& path, Access access, DWORD& error)
{
    const bool writable = access == Access::ReadWriteExclusive;
    HANDLE handle = CreateFileW(path.c_str(), writable ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
                                writable ? 0 : FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return nullptr;
    }
    error = ERROR_SUCCESS;
    return std::unique_ptr<File>(new File(handle, path, writable));
}

std::unique_ptr<File> File::CreateNew(const std::wstring& path, DWORD& error)
{
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return nullptr;
    }
    error = ERROR_SUCCESS;
    return std::unique_ptr<File>(new File(handle, path, true));
}

File::File(HANDLE handle, std::wstring path, bool writable)
    : m_handle(handle), m_path(std::move(path)), m_writable(writable)
{
}

File::~File() { CloseHandle(m_handle); }

bool File::ReadAt(uint64_t offset, void* buffer, size_t length)
{
    auto* p = static_cast<uint8_t*>(buffer);
    while (length > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min(length, kMaxChunk));
        OVERLAPPED ov = OffsetOverlapped(offset);
        DWORD read = 0;
        if (!ReadFile(m_handle, p, chunk, &read, &ov)) {
            return false;
        }
        if (read != chunk) {
            SetLastError(ERROR_HANDLE_EOF);
            return false;
        }
        p += chunk;
        offset += chunk;
        length -= chunk;
    }
    return true;
}

bool File::WriteAt(uint64_t offset, const void* buffer, size_t length)
{
    const auto* p = static_cast<const uint8_t*>(buffer);
    while (length > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min(length, kMaxChunk));
        OVERLAPPED ov = OffsetOverlapped(offset);
        DWORD written = 0;
        if (!WriteFile(m_handle, p, chunk, &written, &ov) || written != chunk) {
            return false;
        }
        p += chunk;
        offset += chunk;
        length -= chunk;
    }
    return true;
}

bool File::GetSize(uint64_t& size) const
{
    LARGE_INTEGER value;
    if (!GetFileSizeEx(m_handle, &value)) {
        return false;
    }
    size = static_cast<uint64_t>(value.QuadPart);
    return true;
}

bool File::SetSize(uint64_t size)
{
    LARGE_INTEGER distance;
    distance.QuadPart = static_cast<LONGLONG>(size);
    return SetFilePointerEx(m_handle, distance, nullptr, FILE_BEGIN) && SetEndOfFile(m_handle);
}

bool File::Flush() { return !m_writable || FlushFileBuffers(m_handle); }

std::wstring FullPath(const std::wstring& path)
{
    const DWORD length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (length == 0) {
        return path;
    }
    std::wstring result(length, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), length, result.data(), nullptr);
    result.resize(written);
    return result;
}

std::wstring DirectoryOf(const std::wstring& path)
{
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator + 1);
}

std::wstring ResolveRelative(const std::wstring& directory, const std::wstring& path)
{
    const bool absolute = (path.size() >= 2 && path[1] == L':') || path.starts_with(L"\\\\") ||
                          path.starts_with(L"\\") || path.starts_with(L"/");
    if (absolute) {
        return path;
    }
    std::wstring relative = path;
    while (relative.starts_with(L".\\") || relative.starts_with(L"./")) {
        relative.erase(0, 2);
    }
    return FullPath(directory + relative);
}

} // namespace tiscsi::disk
