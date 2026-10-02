Mufus: Multi-instance Rufus
===========================

[![Latest Release](https://img.shields.io/github/v/release/ExecSam/Mufus?style=flat-square&label=Latest%20Release)](https://github.com/ExecSam/Mufus/releases/latest)
[![Licence](https://img.shields.io/badge/license-GPLv3-blue.svg?style=flat-square&label=License)](https://www.gnu.org/licenses/gpl-3.0.en.html)

**Mufus** is [Rufus](https://rufus.ie), the Reliable USB Formatting Utility by
[Pete Batard](https://github.com/pbatard/rufus), with one addition: it can write the same image to
**up to 10 USB drives at once**.

Everything else works exactly like Rufus: the regular single drive mode is unchanged, and so are all
the Rufus features listed below. The multi-drive mode is by Sam Jackaman ([ExecSam](https://github.com/ExecSam)).

> Mufus is an independent fork. It is not affiliated with, or supported by, the Rufus project or Akeo
> Consulting. Please report Mufus issues [here](https://github.com/ExecSam/Mufus/issues), not to Rufus.

Download
--------

Get the latest version from the [Releases](https://github.com/ExecSam/Mufus/releases/latest) page:

| File                  | For                                                   |
|-----------------------|-------------------------------------------------------|
| `mufus-<version>.exe`     | 64-bit Windows (x64) - this is the one most people want |
| `mufus-<version>_x86.exe` | 32-bit Windows                                    |

* **Requirements:** Windows 8 or later, and administrator rights (Mufus asks for them when it starts, as
  Rufus does).
* **No installation:** just run the executable.
* **Windows SmartScreen:** Mufus releases are not code signed (unlike the official Rufus releases), so
  Windows may say *"Windows protected your PC"* the first time you run it. Click **More info** then
  **Run anyway**. If you'd rather not, you can build Mufus yourself (see below), and the SHA-256 of each
  release file is listed in its release notes.

Writing to multiple drives
--------------------------

1. Plug in the drives you want to write to.
2. Click the **multi-drive** button (stacked drives icon) at the end of the **Device** row, tick the drives
   you want (up to 10) and click **OK**. The device dropdown now shows your selection (e.g.
   *3 drives selected: E:, F:, G:*). Click it to change the selection, or click the multi-drive button
   again to go back to single drive mode.
3. Select your image and options as usual, then press **START**. All the usual Rufus questions (ISO or DD
   mode, Windows customization options, additional downloads, ...) are asked **once**, for all the
   drives. You then get one confirmation listing every drive that is about to be erased.
4. The drives are written **at the same time**. A progress window shows the status of each drive, the
   main progress bar shows the overall progress, and the log shows the output of every drive, prefixed
   with its drive letter (or disk number).
5. When it's done, the progress window shows which drives succeeded. Any drive that failed is listed
   with the reason, and the others are not affected.

Good to know:

* **Same image and options for every drive.** Options that Rufus derives from the drive itself, such as
  the default cluster size or the size based volume label, are worked out for each drive. Every drive is
  checked (size, file system, sector size, ...) before anything is written, and the options shown are
  the ones for the smallest selected drive.
* **Cancel** stops all the drives.
* **Speed:** the drives share your USB controller(s) and hub(s), so writing 10 drives at once is faster
  than writing them one by one, but each drive will be slower than on its own. A powered USB 3 hub, or
  spreading the drives over several ports, helps.
* **Saving a drive to an image** (the save button next to the device list) is single drive only.
* **Mufus and Rufus can't run at the same time.** They change the same Windows settings while running
  (and restore them on exit), so each one refuses to start while the other is open.
* **Settings** are stored separately from Rufus' (in `HKCU\Software\ExecSam\Mufus`, or in a `mufus.ini`
  file next to the executable, if there is one).
* **Updates:** Mufus doesn't check for new versions of itself, so check the
  [Releases](https://github.com/ExecSam/Mufus/releases) page from time to time. The *Check for updates*
  setting is still used for the rest (UEFI revocation list updates and Windows ISO downloads).
* **Languages:** Mufus has all the Rufus translations, but the few multi-drive mode texts are in English.

Features (from Rufus)
---------------------

* Format USB, flash card and virtual drives to FAT/FAT32/NTFS/UDF/exFAT/ReFS/ext2/ext3
* Create DOS bootable USB drives using [FreeDOS](https://www.freedos.org) or MS-DOS
* Create BIOS or UEFI bootable drives, including [UEFI bootable NTFS](https://github.com/pbatard/uefi-ntfs)
* Create bootable drives from bootable ISOs (Windows, Linux, etc.)
* Create bootable drives from bootable disk images, including compressed ones
* Create Windows 11 installation drives for PCs that don't have TPM or Secure Boot
* Create [Windows To Go](https://en.wikipedia.org/wiki/Windows_To_Go) drives
* Create VHD/DD, VHDX and FFU images of an existing drive
* Create persistent Linux partitions
* Compute MD5, SHA-1, SHA-256 and SHA-512 checksums of the selected image
* Perform runtime validation of UEFI bootable media
* Improve Windows installation experience by automatically setting up OOBE parameters (local account, privacy options, etc.)
* Perform bad blocks checks, including detection of "fake" flash drives
* Download official Microsoft Windows 8, Windows 10 or Windows 11 retail ISOs
* Download [UEFI Shell](https://github.com/pbatard/UEFI-Shell) ISOs
* Modern and familiar UI, in 38 languages
* Small footprint. No installation required.
* Portable. Secure Boot compatible.
* 100% [Free Software](https://www.gnu.org/philosophy/free-sw) ([GPL v3](https://www.gnu.org/licenses/gpl-3.0))

For help with these, see the [Rufus FAQ](https://github.com/pbatard/rufus/wiki/FAQ): it applies to Mufus too.

How it works
------------

Rufus keeps most of its state in global variables, so running several copies of its formatting code in
the same process is not possible. Instead, Mufus performs all of the preparation work in the main
application, and then starts one hidden copy of itself per drive, each running the unmodified Rufus
formatting code against its own drive. Their logs, progress, prompts and results are relayed to the main
application. Resources that the drives would otherwise compete for (drive letters, temporary files,
offline registry hives, disk signatures, VHD sources, ...) are coordinated between them.

Building
--------

Use Visual Studio 2026 (or 2022) or MinGW, and build the `.sln` or `configure`/`make` respectively, as
for Rufus. For example, from a *Developer Command Prompt for VS 2022*:

```
msbuild rufus.sln /m /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
```

The executable is `x64\Release\mufus.exe` (`/p:Platform=x86` for 32-bit). You are entitled to use the
freely available [Visual Studio Community Edition](https://www.visualstudio.com/vs/community/) to build
Mufus.

The end-to-end tests of the multi-drive mode, which write to virtual disks, are described in
[tests/e2e](tests/e2e/README.md).

Credits and license
-------------------

* Rufus is Copyright © 2011-2026 [Pete Batard](https://github.com/pbatard) and its many contributors.
  See [rufus.ie](https://rufus.ie) and the About box for the full list of credits and licenses.
* The multi-instance (multi-drive) functionality is Copyright © 2026 Sam Jackaman ([ExecSam](https://github.com/ExecSam)).
* Like Rufus, Mufus is [Free Software](https://www.gnu.org/philosophy/free-sw), licensed under the
  [GPL v3](https://www.gnu.org/licenses/gpl-3.0) or later. The full source code is in this repository.
