# Tests

End-to-end tests that start `TISCSICli.exe` and talk to it over iSCSI with a
small Python initiator (`initiator.py`). They need Python 3.8+ on Windows and
no extra packages.

```bat
python tests\run_tests.py
python tests\run_tests.py --exe bin\Win32\Release\TISCSICli.exe
python tests\run_tests.py -k protocol
python tests\run_tests.py -k verify
python tests\run_tests.py -k synchronize_cache
python tests\run_tests.py -k vhdx
```

VERIFY tests cover READ-style medium verification and byte comparisons for
VERIFY (10), (12) and (16), using RAM, writable/read-only raw images and ISO
images. They check multi-burst Data-Out, zero length, range overflow,
unsupported flags, mismatches across read chunks and unchanged disk contents.

SYNCHRONIZE CACHE (10)/(16) tests cover RAM, writable/read-only raw images,
ISO images, range requests and invalid LUNs. Both commands synchronously
flush the whole LUN through `Disk::Flush`, including requests for a smaller
range. The native test checks that the backend is called and that flush
errors reach the initiator as MEDIUM ERROR / WRITE FAULT (03/03/00).

The separate `verify_target.cpp` test injects flush errors, read errors and
short reads through `Disk`. It also checks bounded reads, 4096-byte sectors
and mismatch sense data for LBAs above 32 bits. From an **x64 Native Tools
Command Prompt**, in the repository root:

```bat
msbuild TISCSI.sln -p:Configuration=Debug -p:Platform=x64
cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /MTd /D_DEBUG /I src tests\verify_target.cpp bin\x64\Debug\Core.lib /Fo:bin\x64\Debug\verify_target.obj /Fe:bin\x64\Debug\verify_target.exe /link /DEBUG
bin\x64\Debug\verify_target.exe
```

VERIFY follows the unprotected-block behavior described in the
[SCSI Commands Reference Manual, sections 3.55-3.57](https://www.seagate.com/files/staticfiles/support/docs/manual/Interface%20manuals/100293068j.pdf).
BYTCHK=0 flushes and reads the range; BYTCHK=1 compares every block with
Data-Out; BYTCHK=3 compares each block with one supplied block. Comparison
data is limited to 32 MiB per command. The read buffer is at most 1 MiB for
the supported sector sizes. Mismatches return MISCOMPARE (0E/1D/00) with
the first differing LBA. Nonzero VRPROTECT, BYTCHK=2 and relative addressing
are rejected. On optical LUNs, bit 2 is MMC's BLKVFY and is rejected rather
than interpreted as SBC's repeated-block comparison.

Image format tests use third-party tools when installed and are skipped
otherwise: `qemu-img` (QEMU) and `vmware-vdiskmanager` (VMware Workstation).
They are looked up in their default install locations or through the
`TISCSI_QEMU_IMG` / `TISCSI_VDISKMANAGER` environment variables.
Differencing VHDs are created by `vhd_tool.py`, an independent spec-based
helper, because the Microsoft tools for that need administrator rights.

VHDX tests compare fixed/dynamic images and written data with QEMU. They also
ask QEMU to replay a BAT log emitted by TISCSI after deliberately damaging
its destination sector. `vhdx_tool.py` creates specification fixtures for
512/4096-byte sectors, partial differencing blocks, multilevel chains,
ZERO/UNDEFINED/UNMAPPED blocks, bitmap boundaries and a sparse 64 TiB disk.
Checks include parent hashes, stale/missing parents, cycles, redundant
headers/tables, malformed offsets, overlapping regions and unknown required
metadata. These fixtures do not replace testing Hyper-V-created checkpoints.

The native failure test intercepts Win32 writes, flushes and file expansion
in the production `File` implementation. It fails each mutation in a block
allocation, with and without a durable partial write, and verifies that the
backend stops I/O after failure. The Python runner then uses QEMU to recover
each snapshot and checks the entire virtual disk against the old and new
contents. From an **x64 Native Tools Command Prompt** after a Debug x64 build:

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /MTd /D_DEBUG /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0501 /I src tests\vhdx_faults.cpp bin\x64\Debug\Core.lib ole32.lib /Fo:bin\x64\Debug\vhdx_faults.obj /Fe:bin\x64\Debug\vhdx_faults.exe /link /DEBUG
python tests\check_vhdx_faults.py --exe bin\x64\Debug\vhdx_faults.exe
```

The runner accepts `--qemu-img PATH` or `TISCSI_QEMU_IMG`. All failure images
are disposable copies in a temporary directory. This tests file-operation
failures and torn writes, not hardware power loss. VHDX layout and journal
rules follow [Microsoft MS-VHDX v8.0](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-vhdx/83e061f8-f6e2-4de1-91bd-5d518a43d477).

Dynamic volume tests use images of LDM disks built by `ldm_tool.py` (every
layout, degraded volumes, damaged header copies).

Optical disc tests serve `.iso` images and files given with `--cdrom`, and
check the CD/DVD-ROM (MMC) commands. `iso_tool.py` writes small ISO 9660 images from the specification,
so no mkisofs is needed.

Configuration file tests serve targets from hand-written and saved files
(UTF-8, UTF-16, ANSI), check errors with line numbers, `--wait` with an image
that appears later, and `--skip-unavailable`. The GUI test saves a
configuration through the file dialog and opens it again with `--config`.
Without administrator rights, the device tests check that every stable name
`--list` shows finds its disk or volume (opening it then fails, except for
removable media).

Access tests cover allowed initiators (also in discovery) and CHAP:
`initiator.py` logs in with one-way and mutual CHAP, and with the mistakes
the target must refuse (no CHAP, skipped security stage, wrong name or
secret, reflected challenge, unsupported algorithm). The elevated optical
test also logs the Microsoft initiator in with one-way CHAP.

The elevated service test installs a service named `TISCSI-Test-<pid>`, serves
a configuration through it, checks its log, updates it, lets it fail on a
broken configuration, and removes it.

Run from an elevated command prompt to include the device tests: they create
and attach a 64 MiB VHD with diskpart, serve it with `--disk`, `--volume`,
`--disk-ro` and `--spti`, and detach it afterwards. The elevated optical test
connects the Microsoft iSCSI Initiator (starting the MSiSCSI service for the
test if it is stopped) to a CD and a DVD image and reads files from the drives
Windows mounts; it removes the portal and the session afterwards. The elevated dynamic
volume test converts three test VHDs to dynamic disks, creates simple,
spanned, striped and mirrored volumes (RAID-5 too on Windows Server), and
compares what Windows and TISCSI read and write through each other. Recent
Windows 11 builds refuse to convert virtual disks to dynamic disks
(VDS_E_NOT_SUPPORTED); the test is skipped there, so run it on Windows 7 / 10
or Windows Server.

The server listens on 127.0.0.1 (port 3270 by default, `--port` to change).
Disk images are created in a temporary directory. The script exits with a
non-zero status if any check fails.

Add a test by writing a `test_*(ctx)` function in `run_tests.py` and listing
it in `TESTS`; use `ctx.check(condition, description)` for assertions and
`Server(ctx, [...TISCSICli arguments...])` as a context manager.
