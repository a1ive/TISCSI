// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Thin wrapper over a Win32 file handle with positional I/O.

#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace tiscsi::disk {

class File {
public:
    enum class Access {
        ReadShared,     // read-only; other readers allowed, writers not
        ReadWriteExclusive,
    };

    static std::unique_ptr<File> Open(const std::wstring& path, Access access, DWORD& error);
    // Fails if the file exists.
    static std::unique_ptr<File> CreateNew(const std::wstring& path, DWORD& error);

    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    const std::wstring& Path() const { return m_path; }
    bool IsWritable() const { return m_writable; }

    // Both fail on a short transfer. Set GetLastError() on failure.
    bool ReadAt(uint64_t offset, void* buffer, size_t length);
    bool WriteAt(uint64_t offset, const void* buffer, size_t length);
    bool GetSize(uint64_t& size) const;
    bool SetSize(uint64_t size);
    bool Flush();

private:
    File(HANDLE handle, std::wstring path, bool writable);

    HANDLE m_handle;
    std::wstring m_path;
    bool m_writable;
};

// Full path of `path`, resolved against the current directory.
std::wstring FullPath(const std::wstring& path);
// Directory part of a path, including the trailing separator ("" if none).
std::wstring DirectoryOf(const std::wstring& path);
// `path` if absolute, otherwise `directory` + `path`.
std::wstring ResolveRelative(const std::wstring& directory, const std::wstring& path);

} // namespace tiscsi::disk
