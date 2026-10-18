# TISCSI

TISCSI is a free, open-source, user-mode iSCSI target server for Windows,
written with the Win32 API and modern C++. It is a port of
[iSCSI Console](https://github.com/TalAloni/iSCSIConsole) by Tal Aloni.

- Native, portable executable: no .NET, no installer
- Runs on Windows XP and later (x86 / x64)
- User interface in English, Japanese, Simplified Chinese, Traditional Chinese
  and Korean

> **Status:** early development. Notes for contributors, including open items,
> are in [AGENTS.md](AGENTS.md).

## Building

Requirements:

- Visual Studio 2026 with the "Desktop development with C++" workload
  (platform toolset v145) and a Windows 10/11 SDK
- NuGet packages [VC-LTL](https://github.com/Chuyu-Team/VC-LTL5) and
  [YY-Thunks](https://github.com/Chuyu-Team/YY-Thunks), restored automatically

Open `TISCSI.sln` in Visual Studio and build, or from a Developer Command Prompt:

```bat
msbuild TISCSI.sln -t:restore -p:RestorePackagesConfig=true
msbuild TISCSI.sln -p:Configuration=Release -p:Platform=x86
msbuild TISCSI.sln -p:Configuration=Release -p:Platform=x64
```

Output goes to `bin\<Platform>\<Configuration>\`.

End-to-end tests (Python 3, see [tests/README.md](tests/README.md)):

```bat
python tests\run_tests.py --exe bin\x64\Release\TISCSICli.exe
```

## Graphical interface

Run `TISCSI.exe`, choose **Add Target...**, add disks (create a VHD or RAM
disk, or add a disk image, physical disk or volume), then **Start**. To serve
an image as a CD/DVD-ROM drive, check **Serve as a CD/DVD-ROM drive** when
adding it (set automatically for `.iso` files). Physical
disks and volumes need TISCSI to run as administrator. Start with `/log` to
write a log file next to the executable.

## Command line

`TISCSICli.exe` serves disk images and RAM disks. Images are recognized by
extension: VHD (fixed, dynamic, differencing), VMDK (sparse, flat, split,
delta), ISO (served as a read-only CD/DVD-ROM drive) and raw images for
anything else. `--cdrom` serves any file as a CD/DVD-ROM regardless of its
extension. Each disk becomes the next LUN of the current target:

```bat
TISCSICli --target iqn.2026-10.example:disk1 --image D:\disk.vhd --ram 1G
TISCSICli --listen 192.168.1.10 --port 3260 --image-ro D:\install.vmdk --log tiscsi.log
TISCSICli --target iqn.2026-10.example:setup --image D:\setup.iso --cdrom D:\drivers.img
TISCSICli --create-vhd D:\new.vhd --size 20G --dynamic --image D:\new.vhd
```

Physical disks, volumes and SCSI pass-through need an elevated command prompt.
`--list` shows what is available:

```bat
TISCSICli --list
TISCSICli --target iqn.2026-10.example:usb --disk 9
TISCSICli --target iqn.2026-10.example:data --volume-ro E:
TISCSICli --target iqn.2026-10.example:cdrom --spti \\.\CdRom0
```

Serving a disk or volume read-write locks and dismounts its volumes until the
server stops.

Disk numbers, device paths and drive letters can change after a restart, on
another computer or in Windows PE. `--list` also shows stable names, which
can be used instead:

| Device | Stable names |
|--------|--------------|
| Physical disk (`--disk`, `--disk-ro`) | `serial:<serial number>`, `gpt:{<disk GUID>}`, `mbr:<disk signature>` |
| Volume (`--volume`, `--volume-ro`) | `gpt:{<partition GUID>}`, `mbr:<disk signature>:<offset in bytes>` |
| SCSI device (`--spti`) | `serial:<serial number>` |

```bat
TISCSICli --target iqn.2026-10.example:pc01 --disk serial:ZDHBR2TW
TISCSICli --target iqn.2026-10.example:data --volume-ro mbr:86C7075B:1048576
```

A name must match exactly one device; a serial number or signature that two
devices share is not offered. The serial number survives repartitioning (an
initiator that reinstalls Windows on a served disk changes its GPT GUID or
MBR signature) but is compared as Windows reports it, which can differ
between Windows versions.

Dynamic volumes (simple, spanned, striped, mirrored, RAID-5) are listed by
`--list` and served with `--dynamic-volume` / `--dynamic-volume-ro`, by GUID,
name or drive letter, including volumes Windows has not mounted (foreign or
offline disk groups, Windows editions without dynamic disk support). They can
also be assembled from images of the member disks, without administrator
rights:

```bat
TISCSICli --target iqn.2026-10.example:raid --dynamic-volume-ro Volume3
TISCSICli --ldm-disk D:\disk1.vhd --ldm-disk D:\disk2.vhd --list
TISCSICli --ldm-disk D:\disk1.vhd --ldm-disk D:\disk2.vhd --dynamic-volume {6a2d1a39-...}
```

Disks given before any `--target` belong to
`iqn.1991-05.com.microsoft:target1`, the same default as iSCSI Console.
Read-write images are opened exclusively. Stop the server with Ctrl+C.
Run `TISCSICli --help` for all options.

## Language

The UI follows the Windows display language. To override it, choose
**View > Language**, set `Language=` under `[UI]` in `TISCSI.ini` next to the
executable, or start with `--lang=<tag>` (both programs): `en-US`, `ja-JP`,
`zh-CN`, `zh-TW` or `ko-KR`.

## License

Copyright (C) 2026 A1ive

TISCSI is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE](LICENSE).

Portions are derived from iSCSI Console, Copyright (C) 2012-2024 Tal Aloni,
licensed under the GNU Lesser General Public License version 3 or later.
