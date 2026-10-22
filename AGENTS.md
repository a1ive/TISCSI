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
- UI languages: en-US, ja-JP, zh-CN, zh-TW, ko-KR, tr-TR.
- Reference sources live in `ref/` (git-ignored): `ref/iSCSIConsole` and
  `ref/DiskAccessLibrary` (the LDM code was ported from the latter). The VHD
  and VMDK code was written from the public format specifications instead.
  VHDX follows Microsoft [MS-VHDX] v8.0; `ref/VirtualBox-7.2.20` was consulted
  for its read-only backend and supported format boundaries.

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
  administrator rights. `iso_tool.py` writes ISO 9660 images.
- `test_devices_elevated` and `test_dynamic_volumes_elevated` need an elevated
  prompt (they attach VHDs with diskpart) and skip otherwise.
  `test_optical_elevated` (also elevated) connects the Microsoft iSCSI
  Initiator to `.iso` LUNs and reads files through CDFS.
  `test_service_elevated` installs a service named `TISCSI-Test-<pid>`
  (never the real `TISCSI`), serves from it and removes it.
- Run the suite on Debug x64 and Release x86. All checks must pass.

## Source layout

| Path | Contents |
|------|----------|
| `src/Common` | Front-end helpers: i18n resource loader, text / error formatting, log file |
| `src/Core/util` | Byte order, blocking queue, logging, iSCSI key=value text |
| `src/Core/config` | `ServerConfig` (targets, disks, server options) shared by the front ends; `OpenDisk`, stable names; configuration files |
| `src/Core/disk` | `Disk` interface, raw / VHD / VHDX / VMDK / ISO images, RAM disk, `OpenDiskImage`, physical disks, volumes, device enumeration, stable device names (`DeviceSelector`) |
| `src/Core/ldm` | LDM database parsing, disk groups, `DynamicVolume`, `ListDynamicVolumes` / `OpenDynamicVolume` |
| `src/Core/scsi` | CDB parsing, SCSI and MMC (CD/DVD) response data, `VirtualScsiTarget`, `SptiTarget` |
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
  SYNCHRONIZE CACHE (10) and (16) synchronously flush the whole LUN.
- VERIFY (10), (12) and (16) check the range, flush pending writes and read
  in bounded chunks. BYTCHK=1 compares the supplied blocks; BYTCHK=3 compares
  one supplied block against the whole range. Mismatches return MISCOMPARE
  with the first differing LBA. Protection information, relative addressing
  and MMC blank verification are unsupported and rejected.
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
- `.iso` images, and any file given with `--cdrom` (GUI: the CD/DVD-ROM
  check box), are served as read-only CD/DVD-ROM drives (iSCSI Console has
  no optical support; its PRs #9 / #10 were never merged and were not
  ported). Every LUN also takes MODE SENSE (10) and READ / WRITE (12).
- Targets can restrict access (`iscsi::TargetAccess`): allowed initiator
  names, also applied to SendTargets, and CHAP / mutual CHAP. iSCSI Console
  let every initiator in and only ever answered `AuthMethod=None`.

## Design notes

- Threads: one accept thread. Per connection, a receive and a sender thread
  using overlapped WSARecv/WSASend that wait on an abort event. One worker per
  target (`ScsiCommandQueue`). One housekeeping thread for NOP-In keep-alive
  and session expiry. `IscsiServer::Stop()` waits for every connection
  thread (live-connection list).
- Images are chosen by extension (`.vhd`, `.vhdx` / `.avhdx`, `.vmdk`,
  `.iso`, else raw). Read-write images are opened exclusively; differencing
  parents are read-only and verified by UniqueId (VHD), DataWriteGuid (VHDX)
  or CID (VMDK).
- VHDX supports fixed, dynamic and differencing images, 512/4096-byte logical
  sectors and up to 64 TiB. Partial child blocks use the sector bitmap for
  reads; writes materialize a full block before journaling its BAT entry.
  Header changes use both copies with CRC32C and flushes. The first BAT
  update per writable session appends a fresh 1 MiB log region, preserving a
  valid checkpoint before publishing its LogGuid. Old log regions remain
  unused. Failed writes latch an error until close. Pending logs are refused
  with ERROR_NOT_SUPPORTED and require recovery by an external VHDX tool.
  `--create-vhd` still creates VHD. `tests/vhdx_tool.py` builds VHDX fixtures;
  `tests/vhdx_faults.cpp` injects file write/flush/resize failures.
- Optical discs: `Disk::IsOpticalDisc()` makes `VirtualScsiTarget` report
  device type 5 and answer MMC commands (`scsi/MmcData`) for a finalized
  single-session disc with one data track. Up to 80 minutes (360000 sectors)
  is a CD, larger is a DVD. The disc is always present: eject requests are
  accepted and ignored, and no media events are reported. `IsoImage` reads
  any file as raw 2048-byte sectors; `OpenDiskImage` picks it for `.iso`,
  `--cdrom` and the GUI check box use it whatever the extension. Verified
  with the Microsoft iSCSI Initiator (XP and current Windows), Linux
  open-iscsi, iPXE `sanboot` and UEFI boot.
- Physical disks: read-write locks and dismounts every volume on the disk
  and asks Windows to re-read the partition table on release. Read-only takes
  no locks.
- Stable device names (`disk/DeviceSelector.h`) replace disk numbers, device
  paths and drive letters, which change across restarts and in Windows PE:
  `serial:` / `gpt:{disk GUID}` / `mbr:<signature>` for disks,
  `gpt:{partition GUID}` / `mbr:<signature>:<offset>` for basic volumes,
  `serial:` for SCSI devices. Windows volume GUIDs are not used, because
  each installation (and each Windows PE boot) assigns its own. A name must
  match exactly one device, and `--list` offers only names that do; serial
  first, since an initiator that repartitions a served disk changes its
  partition table identity. Names are resolved when the disk is opened.
- Configuration files (`config/ConfigFile.h`) use the CLI option names as
  keys, and `ConfigBuilder` applies both, so a new server or disk option
  needs only a key there (plus the usage text). Saving converts device names
  to stable names (`UseStableName`) and paths to relative ones; loading
  resolves paths against the file's folder. The GUI keeps a `DiskSpec` per
  LUN (stable names taken when the disk is chosen, since numbers can change
  before saving) and the file's other settings in `App::settings`, which it
  does not use but saves again. Access settings are set when a target is
  added (`AccessDialog`, checked like `config::Validate`). Existing targets
  keep theirs: login threads read `IscsiTarget::access` without a lock.
- Startup (CLI): a target opens with all its disks or not at all, so LUN
  numbers never shift. `--wait` retries failures that may go away
  (`MayAppearLater`: missing device, volume or file) once a second, and a
  `WSAEADDRNOTAVAIL` listening address; other errors fail at once.
- Service (`TISCSICli/Service.cpp`): `--install-service` registers
  `TISCSICli --service --config <absolute path>` (LocalSystem, auto start,
  depends on Tcpip, restart on failure). The service reports running at
  once, so waiting for devices does not hold up the service controller, and
  stops with `Run`'s exit code as the service-specific error. It has no
  console: `cli::SetOutputSink` sends the console output to the log, in
  English.
- CHAP (`iscsi/Chap.cpp`) runs per connection in the security stage: the
  target answers T=0 until CHAP_N / CHAP_R check out, refuses initiators
  that skip the stage or reflect its challenge, and removes a new session
  at once on failure (otherwise it would count until Time2Retain). MD5 and
  random numbers come from CryptoAPI. Secrets never go to the log.
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
  a file instead. Python's text mode also turns the CRLF of `.rc2` and
  `.vcxproj` files into LF, so open them with `newline=""`.
- **rc.exe accepts raw newlines inside strings silently**, so check `.rc2`
  files for raw newlines or tabs inside quotes. The CLI usage text uses
  `\n\` line continuations, which are fine.
- **rc.exe limits a string literal to about 4 KB** (RC2102), hence the CLI
  usage text in two strings (`IDS_CLI_USAGE`, `IDS_CLI_USAGE_MORE`).
- **Test harness quirks:**
  - `CreateProcess` (via Python) rejects relative paths containing `/`, so
    `--exe` is made absolute.
  - vmware-vdiskmanager type 6 is remote-only.
  - diskpart prints in the OEM code page, so decode with `oem`.
  - Volumes on removable media (USB sticks) open read-only without
    administrator rights, so an unprivileged `--volume-ro` can start
    serving; `probe_server` stops it again.
  - The Vista+ file dialogs have no control 1148 for the file name: it is
    an Edit inside a ComboBox (`Gui.file_dialog_name_box`).
  - The only button of an `MB_OK` message box has the ID `IDCANCEL`.
  - Window screenshots taken with `PrintWindow` right after opening a dialog
    can catch it half-painted; wait about 2 s.
- **Windows 11 (26100) refuses `convert dynamic` on attached VHDs**
  (VDS_E_NOT_SUPPORTED, 0x80042400). The elevated LDM test skips there.
- **`New-VHD` needs administrator rights.** Use diskpart in elevated tests,
  or the spec-based helpers otherwise.
- **WSL2 (NAT networking) cannot reach the Windows loopback**, and listening
  on the WSL interface triggers a firewall prompt. To test with Linux
  open-iscsi, run `socat TCP-LISTEN:3270,bind=127.0.0.1,fork EXEC:<script>`
  in WSL, where the script runs a Windows `python.exe` that relays its stdin
  and stdout to 127.0.0.1:3270 (socat splits `EXEC:` arguments at `:`, so
  keep the `C:\` path inside the script).

## Open items

- Run `test_dynamic_volumes_elevated` on Windows 7 / 10 or Windows Server
  (Server also allows RAID-5). It has never run against Windows-made dynamic
  disks; so far only `ldm_tool.py` images are covered.
- GUI flows that need administrator rights are not automated: adding
  physical disks, volumes and unmounted dynamic volumes.
- CHAP is checked against this repository's test initiator and (elevated
  optical test) the Microsoft initiator's one-way CHAP; mutual CHAP with
  the Microsoft or Linux initiator has not been tried.
- Manual checks on XP / Windows 7, including the CJK font fallback (SimSun,
  PMingLiU, Gulim on XP).
- Not supported: compressed (streamOptimized) VMDK, VMFSSPARSE / SESPARSE,
  raw device mappings, VHDX log replay or VHDX creation.
- VHDX differencing and 4K-sector coverage uses specification fixtures.
  Hyper-V-created checkpoint chains and XP / Windows 7 runtime behavior
  need maintainer validation.
