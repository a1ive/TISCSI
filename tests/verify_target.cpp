// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
// Fault-injection tests for VERIFY and SYNCHRONIZE CACHE; see tests/README.md.

#include "Core/scsi/VirtualScsiTarget.h"
#include "Core/util/ByteOrder.h"

#include <cstdio>
#include <cstdlib>

using namespace tiscsi;

namespace {

void Check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what);
        std::exit(1);
    }
}

class ProbeDisk final : public disk::Disk {
public:
    uint32_t blockSize = 4096;
    uint64_t sectors = (uint64_t{1} << 33);
    disk::IoResult flushResult = disk::IoResult::Ok;
    disk::IoResult readResult = disk::IoResult::Ok;
    unsigned failRead = 0;
    bool shortRead = false;
    unsigned reads = 0;
    unsigned flushes = 0;
    uint64_t nextLba = 0;
    uint64_t readBlocks = 0;

    uint32_t BytesPerSector() const override { return blockSize; }
    uint64_t Size() const override { return sectors * blockSize; }
    bool IsReadOnly() const override { return true; }
    std::wstring Description() const override { return L"VERIFY fault injection"; }
    disk::IoResult Flush() override { ++flushes; return flushResult; }
    disk::IoResult ReadSectors(uint64_t lba, uint32_t count, std::vector<uint8_t>& out) override
    {
        Check(flushes == 1, "flush precedes all reads, including on read-only LUNs");
        Check(lba == nextLba, "contiguous reads preserve the full LBA");
        Check(count != 0 && uint64_t{count} * blockSize <= 1024 * 1024, "bounded read buffers");
        ++reads;
        nextLba += count;
        readBlocks += count;
        if (reads == failRead) {
            return readResult;
        }
        out.assign(static_cast<size_t>(count) * blockSize - (shortRead ? 1 : 0), 0x5A);
        return disk::IoResult::Ok;
    }
    disk::IoResult WriteSectors(uint64_t, std::span<const uint8_t>) override
    {
        Check(false, "VERIFY and SYNCHRONIZE CACHE must never write sectors");
        return disk::IoResult::WriteError;
    }
    void Reset(uint64_t lba)
    {
        reads = flushes = 0;
        readBlocks = 0;
        nextLba = lba;
        failRead = 0;
        shortRead = false;
        flushResult = readResult = disk::IoResult::Ok;
    }
};

scsi::CdbBytes Command(uint8_t op, uint64_t lba, uint32_t count, uint8_t flags = 0)
{
    scsi::CdbBytes cdb{};
    cdb[0] = op;
    cdb[1] = flags;
    if (op == scsi::op::Verify16 || op == scsi::op::SynchronizeCache16) {
        util::WriteBE64(cdb.data() + 2, lba);
        util::WriteBE32(cdb.data() + 10, count);
    } else {
        util::WriteBE32(cdb.data() + 2, static_cast<uint32_t>(lba));
        if (op == scsi::op::Verify10 || op == scsi::op::SynchronizeCache10) {
            util::WriteBE16(cdb.data() + 7, static_cast<uint16_t>(count));
        } else {
            util::WriteBE32(cdb.data() + 6, count);
        }
    }
    return cdb;
}

} // namespace

int main()
{
    auto disk = std::make_unique<ProbeDisk>();
    ProbeDisk& probe = *disk;
    std::vector<std::unique_ptr<disk::Disk>> disks;
    disks.push_back(std::move(disk));
    scsi::VirtualScsiTarget target(std::move(disks), {});
    scsi::Bytes response;
    for (const uint8_t op : {scsi::op::SynchronizeCache10, scsi::op::SynchronizeCache16}) {
        const bool wide = op == scsi::op::SynchronizeCache16;
        const uint64_t lba = wide ? (uint64_t{1} << 32) + 7 : 7;
        const uint32_t count = wide ? 65537 : 8;
        auto cdb = Command(op, lba, count);
        cdb[wide ? 15 : 9] = 0x80; // vendor-specific control bit, to check the parsed offset
        const auto parsed = scsi::Cdb::Parse(cdb);
        Check(parsed && parsed->logicalBlockAddress == lba && parsed->transferLength == count &&
              parsed->control == 0x80, "SYNCHRONIZE CACHE preserves CDB field widths and offsets");
        for (const auto result : {disk::IoResult::Ok, disk::IoResult::WriteError}) {
            probe.Reset(lba);
            probe.flushResult = result;
            response.assign(42, 0xCC);
            const auto status = target.ExecuteCommand(cdb, {}, {}, 0, response);
            Check(probe.flushes == 1 && probe.reads == 0, "SYNCHRONIZE CACHE calls Flush exactly once");
            if (result == disk::IoResult::Ok) {
                Check(status == scsi::ScsiStatus::Good && response.empty(), "flush success returns GOOD without data");
            } else {
                Check(status == scsi::ScsiStatus::CheckCondition && response.size() == 20 &&
                      util::ReadBE16(response.data()) == 18 && (response[4] & 0x0F) == 3 &&
                      response[14] == 3 && response[15] == 0,
                      "flush failure returns framed MEDIUM ERROR / WRITE FAULT");
            }
        }
    }
    std::puts("PASS SYNCHRONIZE CACHE(10/16): CDB fields, backend Flush, success and failure responses");
    for (const uint8_t op : {scsi::op::Verify10, scsi::op::Verify12, scsi::op::Verify16}) {
        const uint64_t lba = op == scsi::op::Verify16 ? (uint64_t{1} << 32) + 7 : 7;
        auto run = [&](uint32_t count, uint8_t flags = 0, std::span<const uint8_t> data = {}) {
            response.assign(42, 0xCC); // successful VERIFY must clear stale response data
            return target.ExecuteCommand(Command(op, lba, count, flags), {}, data, 0, response);
        };
        auto sense = [&](uint8_t key, uint8_t asc) {
            return response.size() == 20 && util::ReadBE16(response.data()) == 18 &&
                (response[4] & 0x0F) == key && response[14] == asc && response[15] == 0;
        };
        probe.Reset(lba);
        Check(run(600) == scsi::ScsiStatus::Good && response.empty(), "medium verification succeeds");
        Check(probe.reads == 3 && probe.readBlocks == 600, "whole range is read, including the last partial chunk");

        probe.Reset(lba);
        probe.flushResult = disk::IoResult::WriteError;
        Check(run(1) == scsi::ScsiStatus::CheckCondition && sense(3, 3), "flush failure is reported");
        Check(probe.reads == 0, "failed flush prevents reads");
        for (const auto result : {disk::IoResult::ReadError, disk::IoResult::OutOfRange}) {
            probe.Reset(lba);
            probe.failRead = 2;
            probe.readResult = result;
            Check(run(600) == scsi::ScsiStatus::CheckCondition &&
                  (result == disk::IoResult::ReadError ? sense(3, 0x11) : sense(5, 0x21)),
                  "later-chunk disk errors are propagated");
            Check(probe.reads == 2, "verification stops on the first read error");
        }
        probe.Reset(lba);
        probe.shortRead = true;
        Check(run(1) == scsi::ScsiStatus::CheckCondition && sense(3, 0x11), "short reads never succeed");
        for (const uint8_t mode : {uint8_t{0}, uint8_t{2}, uint8_t{6}}) {
            probe.Reset(lba);
            Check(run(0, mode) == scsi::ScsiStatus::Good && probe.flushes == 0 && probe.reads == 0,
                  "zero length does no disk I/O in any supported mode");
        }
        for (const uint8_t flags : {uint8_t{4}, uint8_t{0x20}, uint8_t{1}}) {
            probe.Reset(lba);
            Check(run(1, flags) == scsi::ScsiStatus::CheckCondition && sense(5, 0x24) && probe.flushes == 0,
                  "bad flags are rejected before disk I/O");
        }
        scsi::Bytes pattern(600 * probe.blockSize, 0x5A);
        probe.Reset(lba);
        Check(run(600, 2, pattern) == scsi::ScsiStatus::Good && probe.readBlocks == 600,
              "4096-byte block comparison spans chunks");
        pattern.back() ^= 1;
        probe.Reset(lba);
        Check(run(600, 2, pattern) == scsi::ScsiStatus::CheckCondition, "last-block mismatch is detected");
        const uint64_t badLba = lba + 599;
        if (badLba > UINT32_MAX) {
            Check(response.size() == 22 && util::ReadBE16(response.data()) == 20 && response[2] == 0x72 &&
                  response[3] == 0x0E && response[4] == 0x1D && response[5] == 0 && response[9] == 12 &&
                  response[10] == 0 && response[11] == 10 && response[12] == 0x80 &&
                  util::ReadBE64(response.data() + 14) == badLba,
                  "descriptor sense preserves the 64-bit mismatch LBA");
        } else {
            Check(sense(0x0E, 0x1D) && response[2] == 0xF0 && util::ReadBE32(response.data() + 5) == badLba,
                  "fixed sense reports the first mismatch LBA");
        }
        probe.Reset(lba);
        pattern.assign(probe.blockSize, 0x5A);
        Check(run(600, 6, pattern) == scsi::ScsiStatus::Good && probe.readBlocks == 600,
              "4096-byte repeated block comparison spans chunks");
        probe.Reset(lba);
        Check(run(1, 2) == scsi::ScsiStatus::CheckCondition && sense(5, 0x24) && probe.flushes == 0,
              "missing comparison payload causes no disk I/O");
    }
    std::puts("PASS VERIFY(10/12/16): bounded I/O, flush/read failures, short reads, 4K sectors, 64-bit sense");
}
