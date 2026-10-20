// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
//
// Deterministic Win32 I/O failure injection for the production VHDX backend.
// Run on a disposable clean dynamic image. Each case gets its own copy.

#include "Core/disk/File.h"
#include "Core/disk/Vhdx.h"

#include <cstdio>

namespace {
unsigned operation = 0, failAt = 0;
bool tornWrite = false;

bool Fail()
{
    if (++operation != failAt) return false;
    SetLastError(ERROR_WRITE_FAULT);
    return true;
}

BOOL WINAPI TestWriteFile(HANDLE file, LPCVOID buffer, DWORD length, LPDWORD written, LPOVERLAPPED overlapped)
{
    if (!Fail()) return WriteFile(file, buffer, length, written, overlapped);
    if (tornWrite && length > 1) {
        // Make a torn prefix durable, including for header/BAT/log writes.
        WriteFile(file, buffer, length / 2, written, overlapped);
        FlushFileBuffers(file);
    }
    SetLastError(ERROR_WRITE_FAULT);
    return FALSE;
}

BOOL WINAPI TestFlushFileBuffers(HANDLE file)
{
    return Fail() ? FALSE : FlushFileBuffers(file);
}

BOOL WINAPI TestSetEndOfFile(HANDLE file)
{
    return Fail() ? FALSE : SetEndOfFile(file);
}
} // namespace

// Compile the real File implementation with only its mutating OS calls
// intercepted. Core.lib then supplies VhdxDisk without pulling File.obj.
#define WriteFile TestWriteFile
#define FlushFileBuffers TestFlushFileBuffers
#define SetEndOfFile TestSetEndOfFile
#include "../src/Core/disk/File.cpp"
#undef WriteFile
#undef FlushFileBuffers
#undef SetEndOfFile

int wmain(int argc, wchar_t** argv)
{
    using namespace tiscsi::disk;
    if (argc != 3) {
        std::fwprintf(stderr, L"Usage: vhdx_faults.exe clean.vhdx output-directory\n");
        return 2;
    }
    CreateDirectoryW(argv[2], nullptr);
    unsigned checks = 0;
    unsigned totalOperations = 0;
    for (unsigned mode = 0; mode < 2; ++mode) {
        for (unsigned fail = 0; fail <= totalOperations; ++fail) {
            const auto path = std::wstring(argv[2]) + L"\\case-" + std::to_wstring(mode) + L"-" +
                              std::to_wstring(fail) + L".vhdx";
            if (!CopyFileW(argv[1], path.c_str(), FALSE)) return 3;
            OpenError error;
            auto disk = VhdxDisk::Open(path, false, error);
            if (!disk) return 4;
            operation = 0;
            failAt = fail;
            tornWrite = mode != 0;
            std::vector<uint8_t> data(disk->BytesPerSector(), 0xD7);
            // The supplied fixture has an absent second payload block.
            const auto result = disk->WriteSectors((2ull << 20) / disk->BytesPerSector(), data);
            const unsigned calls = operation;
            if (fail == 0) {
                if (result != IoResult::Ok || disk->Flush() != IoResult::Ok) return 5;
                totalOperations = calls;
            } else {
                if (result != IoResult::WriteError || disk->Flush() != IoResult::WriteError) return 6;
                std::vector<uint8_t> out;
                if (disk->ReadSectors(0, 1, out) != IoResult::ReadError ||
                    disk->WriteSectors(0, data) != IoResult::WriteError) return 7;
            }
            ++checks;
            failAt = 0;
            disk.reset();
            auto reopened = VhdxDisk::Open(path, true, error);
            if (reopened) {
                std::vector<uint8_t> out;
                if (reopened->ReadSectors((2ull << 20) / reopened->BytesPerSector(), 1, out) != IoResult::Ok ||
                    (out != data && out != std::vector<uint8_t>(out.size()))) return 8;
                std::printf("CLEAN %u %u\n", mode, fail);
            } else if (error.code == ERROR_NOT_SUPPORTED) {
                std::printf("RECOVER %u %u\n", mode, fail);
            } else {
                std::printf("CORRUPT %u %u error=%lu\n", mode, fail, error.code);
                return 9;
            }
            ++checks;
        }
    }
    std::printf("PASS %u failure/reopen checks (%u mutation operations per write)\n", checks, totalOperations);
    return 0;
}
