# Tests

End-to-end tests that start `TISCSICli.exe` and talk to it over iSCSI with a
small Python initiator (`initiator.py`). They need Python 3.8+ on Windows and
no extra packages.

```bat
python tests\run_tests.py
python tests\run_tests.py --exe bin\Win32\Release\TISCSICli.exe
python tests\run_tests.py -k protocol
```

Image format tests use third-party tools when installed and are skipped
otherwise: `qemu-img` (QEMU) and `vmware-vdiskmanager` (VMware Workstation).
They are looked up in their default install locations or through the
`TISCSI_QEMU_IMG` / `TISCSI_VDISKMANAGER` environment variables.
Differencing VHDs are created by `vhd_tool.py`, an independent spec-based
helper, because the Microsoft tools for that need administrator rights.

Dynamic volume tests use images of LDM disks built by `ldm_tool.py` (every
layout, degraded volumes, damaged header copies).

Run from an elevated command prompt to include the device tests: they create
and attach a 64 MiB VHD with diskpart, serve it with `--disk`, `--volume`,
`--disk-ro` and `--spti`, and detach it afterwards. The elevated dynamic
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
