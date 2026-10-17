// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 A1ive
// Derived from DiskAccessLibrary (LogicalDiskManager), Copyright (C) 2014-2018
// Tal Aloni <tal.aloni.il@gmail.com>, licensed under the GNU LGPL version 3 or later.
//
// Dynamic disks and the disk groups they form.

#pragma once

#include "Core/disk/Disk.h"
#include "Core/ldm/LdmDatabase.h"

#include <memory>
#include <optional>
#include <vector>

namespace tiscsi::ldm {

struct DynamicDisk {
    std::shared_ptr<disk::Disk> disk;
    PrivateHeader privateHeader;
    TocBlock toc;
    Guid diskGuid;
    Guid diskGroupGuid;
};

// True if the disk is partitioned as a dynamic disk (MBR partition type 0x42,
// GPT LDM metadata and data partitions, or a PRIVHEAD at sector 6 of a disk
// with no partition table).
bool IsDynamicDisk(disk::Disk& disk);

// Reads the private header and table of contents. Returns nullopt if the disk
// is not a dynamic disk or its LDM headers are damaged.
std::optional<DynamicDisk> ReadDynamicDisk(std::shared_ptr<disk::Disk> disk);

// Reads the volume manager database (the config region) of one disk.
std::optional<Database> ReadDatabase(const DynamicDisk& disk);

struct DiskGroup {
    Guid guid;
    std::string name;
    Database database;
    std::vector<DynamicDisk> disks; // the members that are present

    const DynamicDisk* FindDisk(const Guid& diskGuid) const;
};

// Groups the disks by disk group and reads each group's database. As in
// DiskAccessLibrary, a group is left out if its database is dirty (an update
// was interrupted) or differs between its disks.
std::vector<DiskGroup> ReadDiskGroups(const std::vector<DynamicDisk>& disks);

} // namespace tiscsi::ldm
