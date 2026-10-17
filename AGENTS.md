# AGENTS.md

Notes for anyone (human or agent) working on TISCSI: how to build and test,
the rules the code follows, what is still open, and the traps already hit.

## Project

TISCSI is a port of [iSCSI Console](https://github.com/TalAloni/iSCSIConsole)
by Tal Aloni (C#, LGPL-3.0-or-later) to the Win32 API and modern C++.

- Native, portable executables (`TISCSI.exe` GUI, `TISCSICli.exe` command
  line): no .NET, no installer, usable from Windows PE.
- Runs on Windows XP and later, x86 and x64, built with current MSVC plus
  [VC-LTL](https://github.com/Chuyu-Team/VC-LTL5) (msvcrt.dll CRT) and
  [YY-Thunks](https://github.com/Chuyu-Team/YY-Thunks).
- License GPL-3.0-or-later. Code translated from iSCSI Console or
  DiskAccessLibrary keeps Tal Aloni's copyright line and notes the LGPL origin
  in its header, below the SPDX line.
- UI languages: en-US, ja-JP, zh-CN, zh-TW, ko-KR.
- Reference sources live in `ref/` (git-ignored): `ref/iSCSIConsole` and
  `ref/DiskAccessLibrary` (the LDM code was ported from the latter). The VHD
  and VMDK code was written from the public format specifications instead.

## Build

Visual Studio 2026 (toolset v145), C++20, `/utf-8`. From a shell:

```bat
msbuild TISCSI.sln -t:restore -p:RestorePackagesConfig=true
msbuild TISCSI.sln -p:Configuration=Release -p:Platform=x86
msbuild TISCSI.sln -p:Configuration=Release -p:Platform=x64
```

Output: `bin\<Platform>\<Configuration>\` (`Win32` for x86). Build all four
configurations (Debug/Release x x86/x64) before committing.

After changing dependencies, check that Release builds still import only
XP-era DLLs plus `msvcrt.dll` and have subsystem version 5.01 (x86) / 5.02
(x64): `dumpbin -imports` / `dumpbin -headers`. Current import sets:

- CLI: KERNEL32, ADVAPI32, ole32, WS2_32, msvcrt
- GUI: the above plus COMCTL32, COMDLG32, USER32, SHELL32

## Test

```bat
python tests\run_tests.py
python tests\run_tests.py --exe bin\Win32\Release\TISCSICli.exe
python tests\run_tests.py -k dynamic_volumes
```

- `tests/initiator.py` is a small iSCSI initiator; `run_tests.py` starts
  `TISCSICli.exe` on 127.0.0.1:3270 and checks protocol behavior, images,
  devices, dynamic volumes and the GUI (driven through window messages).
- Image tests use qemu-img and vmware-vdiskmanager when installed
  (`TISCSI_QEMU_IMG` / `TISCSI_VDISKMANAGER` override the paths) and are
  skipped otherwise. `vhd_tool.py` (differencing VHDs) and `ldm_tool.py` (LDM
  disk images: every layout, MBR and GPT, multi-VBLK records, damaged header
  copies) are spec-based helpers for what those tools cannot make without
  administrator rights.
- `test_devices_elevated` and `test_dynamic_volumes_elevated` need an elevated
  prompt (they attach VHDs with diskpart) and skip otherwise.
- Run the suite on Debug x64 and Release x86. All checks must pass.

## Source layout

| Path | Contents |
|------|----------|
| `src/Common` | Front-end helpers: i18n resource loader, text / error formatting, log file |
| `src/Core/util` | Byte order, blocking queue, logging, iSCSI key=value text |
| `src/Core/disk` | `Disk` interface, raw / VHD / VMDK images, RAM disk, `OpenDiskImage`, physical disks, volumes, device enumeration |
| `src/Core/ldm` | LDM database parsing, disk groups, `DynamicVolume`, `ListDynamicVolumes` / `OpenDynamicVolume` |
| `src/Core/scsi` | CDB parsing, SCSI response data, `VirtualScsiTarget`, `SptiTarget` |
| `src/Core/iscsi` | PDUs, sessions, connections, `IscsiTarget`, `IscsiServer` |
| `src/TISCSICli` | Command-line front end |
| `src/TISCSI` | GUI front end |
| `msbuild/` | Shared property sheets (named so because `build/` is git-ignored) |

## Conventions

### Compatibility

- `_WIN32_WINNT` is pinned to `0x0501`, so Vista+ declarations are not
  visible. Use a newer API only if YY-Thunks covers it, and raise the define
  locally.
- No `std::filesystem` or other library features that need Vista+ APIs
  YY-Thunks does not cover; use Win32 file APIs.
- XP / Windows 7 compatibility is tested manually by the maintainer.

### Internationalization

- Every user-visible string lives in resources. Log messages
  (`util::Logger`) stay English on purpose.
- Resources: `src/<app>/lang/<tag>.rc2`, UTF-8 with `#pragma code_page(65001)`,
  included from the app's `.rc`. Every language defines every string and
  dialog (English is also the runtime fallback); compare the `IDS_` counts
  per file after adding strings.
- Lookups go through `Common/I18n.h` (`LoadStr`, `Format`, `LoadMenuL`,
  `DialogBoxParamL`, ...) with an explicit LANGID, which works the same on XP
  (no `SetThreadUILanguage`).
- `std::format` positional arguments (`{0}`, `{1}`) so translations can
  reorder them.
- Language precedence: `--lang` option, then `[UI] Language=` in `TISCSI.ini`
  next to the executable, then the Windows UI language. Chinese is matched by
  script (zh-HK / zh-MO pick zh-TW). The GUI switches languages live by
  rebuilding the main dialog; the server state lives in `App`.
- Adding a language: an entry in `kLanguages` (`src/Common/I18n.cpp`), a
  `lang/<tag>.rc2` in both apps, the `#include` in both `.rc` files, and the
  tag in the usage texts (`IDS_CLI_USAGE`, `IDS_USAGE`) of every language.
- Dialog templates: English uses `MS Shell Dlg` 8. CJK templates are wider
  (Japanese layout) and name a font plus charset explicitly: MS UI Gothic
  (0x80), Microsoft YaHei (0x86), Microsoft JhengHei (0x88), Malgun Gothic
  (0x81). The charset makes XP fall back to its own font for the script.
  Translations use Microsoft's terminology for each language.

### Code

- Match the surrounding code: naming, comment density, file headers
  (SPDX line first).
- Disk implementations are called from one thread at a time (each target has
  one worker thread). Anything shared between targets must lock (see
  `SharedImage` in `ldm/DynamicVolumes.cpp`).

## Intentional differences from iSCSI Console

These are deliberate. Don't "fix" them back to the original behavior.

- CmdSN ordering uses RFC 1982 serial arithmetic (the original computed
  `2 ^ 31 - 1`, an XOR in C#).
- At most `MaxConnections` connections per session (the original allowed one
  more).
- Data-Out PDUs are bounds-checked. R2Ts use the stored transfer tag and ask
  for at most `MaxBurstLength`. Data-In never exceeds
  `ExpectedDataTransferLength`.
- READ/WRITE (6) with length 0 means 256 blocks. Writes to read-only LUNs
  return framed sense data. Failed reads report the full residual.
  SYNCHRONIZE CACHE flushes.
- The power consumption subpage carries its subpage code. Final Text
  Responses carry the reserved TTT. Malformed numeric login keys are ignored.
- Each target gets its own NAA identifier and unit serial (hash of the target
  name); the INQUIRY vendor is `TISCSI`.
- Read-write access to a dynamic disk as a physical disk is refused (iSCSI
  Console asked; a command line cannot).
- Read-write access to a dynamic volume with a missing member is refused,
  because writes would leave the members inconsistent without the LDM
  database recording it.
- Dynamic volumes can be assembled from disk images of the member disks
  (`--ldm-disk`), with no administrator rights needed.
- SPTI passes the initiator's expected read length through the SCSI target
  interface instead of parsing CDBs; REPORT LUNS is answered locally.

## Design notes

- Threads: one accept thread. Per connection, a receive and a sender thread
  using overlapped WSARecv/WSASend that wait on an abort event. One worker per
  target (`ScsiCommandQueue`). One housekeeping thread for NOP-In keep-alive
  and session expiry. `IscsiServer::Stop()` waits for every connection
  thread (live-connection list).
- Images are chosen by extension (`.vhd`, `.vmdk`, else raw). Read-write
  images are opened exclusively; differencing parents are read-only and are
  verified by UniqueId (VHD) or CID (VMDK).
- Physical disks: read-write locks and dismounts every volume on the disk
  and asks Windows to re-read the partition table on release. Read-only takes
  no locks.
- Volumes are served through the OS volume handle, which also covers
  dynamic volumes Windows has mounted.
- Dynamic volumes: the LDM volume GUID is the Windows volume GUID. A volume
  Windows has goes through `VolumeDisk`; others are assembled from member
  disks (degraded mirror reads a remaining plex, degraded RAID-5 rebuilds the
  missing column). TISCSI never writes the LDM database.

## Pitfalls already hit

- **VS 2026 ignores `NuGetImportAfterCppProps`**, so VC-LTL's `config.props`
  was silently not applied and the static CRT got linked.
  `msbuild/Packages.props` imports it directly. If `msvcrt.dll` disappears
  from the imports, look here first.
- **Windows `shutdown()` does not unblock a blocking `recv`.** Stopping the
  server with live sessions hung. Socket I/O is overlapped, with an abort
  event and `CancelIo`.
- **A locked volume is accessible only through the locking handle.**
  `VolumeDisk` locks and dismounts through its own I/O handle
  (`RawDevice::LockAndDismount`), not a second handle.
- **Vista+ blocks raw writes to sectors of mounted volumes**, hence lock plus
  dismount. Writes to sectors of unmounted volumes (e.g. dynamic volumes
  Windows does not know) are allowed.
- **Non-cached device I/O needs aligned buffers.** `RawDevice` uses
  page-aligned bounce buffers.
- **Resource IDs can clash with Windows headers:** `IDC_SIZE` is a
  winuser.h cursor, hence `IDC_SIZE_MB`.
- **Status bar layout:** its height is 0 before the first `WM_SIZE`, and
  `SetMenu` shrinks the client area. Compute the target client size and
  resize twice.
- **CJK fonts:** Gulim and PMingLiU are optional on Windows 10/11. Naming
  them showed Korean as boxes, so use Malgun Gothic / Microsoft JhengHei.
  Korean fonts draw `\` as ₩; that is expected.
- **`FormatMessage` language:** try the UI language first, otherwise Win32
  errors come out in the system language.
- **Shell heredocs mangle backslashes:** Python fed through a Bash heredoc
  turned `\n` in `.rc2` strings into raw line breaks. Write such scripts to
  a file instead.
- **rc.exe accepts raw newlines inside strings silently**, so check `.rc2`
  files for raw newlines or tabs inside quotes. The CLI usage text uses
  `\n\` line continuations, which are fine.
- **Test harness quirks:**
  - `CreateProcess` (via Python) rejects relative paths containing `/`, so
    `--exe` is made absolute.
  - vmware-vdiskmanager type 6 is remote-only.
  - diskpart prints in the OEM code page, so decode with `oem`.
  - Window screenshots taken with `PrintWindow` right after opening a dialog
    can catch it half-painted; wait about 2 s.
- **Windows 11 (26100) refuses `convert dynamic` on attached VHDs**
  (VDS_E_NOT_SUPPORTED, 0x80042400). The elevated LDM test skips there.
- **`New-VHD` needs administrator rights.** Use diskpart in elevated tests,
  or the spec-based helpers otherwise.

## Open items

- Run `test_dynamic_volumes_elevated` on Windows 7 / 10 or Windows Server
  (Server also allows RAID-5). It has never run against Windows-made dynamic
  disks; so far only `ldm_tool.py` images are covered.
- GUI flows that need administrator rights are not automated: adding
  physical disks, volumes and unmounted dynamic volumes.
- Manual checks on XP / Windows 7, including the CJK font fallback (SimSun,
  PMingLiU, Gulim on XP).
- Not supported: compressed (streamOptimized) VMDK, VMFSSPARSE / SESPARSE,
  raw device mappings, VHDX.
