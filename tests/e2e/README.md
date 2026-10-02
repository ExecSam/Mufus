Mufus end-to-end tests
======================

These tests exercise the real Mufus UI against virtual disks, so that multi-drive mode can be
validated without risking any physical drive.

`mufus-e2e.ps1`:

1. Creates and attaches one VHDX per size in `-SizesMB` (default: 1, 2 and 3 GB).
2. Starts Mufus and drives its UI through window messages: selects the options, enables
   multi-drive mode, selects all the drives, presses START and answers the prompts.
3. Verifies the content of every disk once the operation completes.
4. Detaches and deletes the VHDX files.

| Test      | What it does                                                         | Verification                                  |
|-----------|----------------------------------------------------------------------|-----------------------------------------------|
| `nonboot` | Non bootable, FAT32, on all drives                                   | FAT32 on each drive, drive specific labels, workers ran concurrently |
| `freedos` | FreeDOS on all drives                                                | `KERNEL.SYS` and `COMMAND.COM` on each drive  |
| `dd`      | DD image (`dd.img`) on all drives                                    | SHA-256 of the raw content of each drive      |
| `iso`     | UEFI ISO (`efi.iso`) in ISO mode on all drives                       | SHA-256 of every file of the ISO, on each drive |
| `isombr`  | Same as `iso`, with the MBR partition scheme (UEFI target)           | Same as `iso`, plus all disks online with distinct signatures |
| `exclusive` | Starts Mufus while Rufus (`-RufusExe`) runs, and the reverse       | The second application refuses to start       |
| `cancel`  | Bad blocks check on all drives, cancelled after a few seconds        | All workers exit, the UI recovers             |
| `single`  | Regular single drive mode (NTFS)                                     | Only one drive is reformatted, no workers     |
| `blank`   | Same as `nonboot`, on wiped (uninitialized) drives                   | Same as `nonboot`                             |
| `ui`      | Screenshots of Mufus (and of upstream Rufus with `-RufusExe`), in light and dark mode: main window, advanced options, About, drive picker, multi-drive progress | Saved in `shots\` for review; the multi-drive write must succeed |
| `win`     | Official Windows ISO (`-WinIso`) on all drives (12, 14 and 16 GB disks by default, see `-WinSizesMB`), with the TPM/Secure Boot/RAM, online account, data collection and BitLocker customizations | Every file of the ISO on each drive (SHA-256), only the expected files altered/added, bypass present in the registry of `boot.wim` |

**Safety:** before writing anything, each test checks that the only non-system disks present are the
test VHDX disks, and aborts otherwise. Unplug any USB drive before running the tests anyway.

Running the tests
-----------------

The test images are generated with Python and [pycdlib](https://pypi.org/project/pycdlib/):

```
pip install pycdlib
python tests\e2e\make_images.py %TEMP%\mufus-test-images
```

Then, from an **elevated** Windows PowerShell 5.1 prompt (which the script is written for), in the
root of the repository:

```
powershell -ExecutionPolicy Bypass -File tests\e2e\mufus-e2e.ps1 -Exe x64\Release\mufus.exe `
    -Images $env:TEMP\mufus-test-images -WorkDir $env:TEMP\mufus-test-work
```

Use `-Tests nonboot,dd` to run a subset of the tests. The report (`report.txt`) and the Mufus log of
each test are saved in the work directory. The `win` test is skipped unless `-WinIso` is provided, and
needs about 3.5 times the size of the ISO in free space on the work drive.
