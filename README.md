Mufus: Multi-instance Rufus
===========================

**Mufus** is a fork of [Rufus](https://rufus.ie), the Reliable USB Formatting Utility by
[Pete Batard](https://github.com/pbatard/rufus), that can write an image to **up to 10 USB drives at once**.

Everything Rufus does, Mufus does the same way: the regular single drive mode is unchanged. Multi-drive
mode is an addition, by Sam Jackaman ([ExecSam](https://github.com/ExecSam)).

Multi-drive mode
----------------

* Click the multi-drive button at the end of the **Device** row, then tick the drives you want to write to
  (up to 10). The device dropdown then shows your selection. Click it again to change the selection, or click
  the multi-drive button again to go back to single drive mode.
* Select your image and options as usual, and press **START**. All the regular Rufus checks and prompts
  (ISO/DD mode, Windows User Experience options, downloads, ...) happen once, for all the drives.
* The drives are then written **concurrently**, and a progress window shows the status of each drive.
  The main progress bar shows the overall progress, and the log shows the output of each drive, prefixed
  with its drive letter (or disk number).
* Options that Rufus derives from the drive (default cluster size, size based volume label, ...) are
  computed for each drive, and every drive is validated (size, file system, sector size, ...) before
  anything is written.

### How it works

Rufus keeps most of its state in global variables, so running several copies of its formatting code in
the same process is not possible. Instead, Mufus performs all of the preparation work in the main
application, and then starts one hidden copy of itself per drive, each running the unmodified Rufus
formatting code against its own drive. Their logs, progress, prompts and results are relayed to the main
application. Resources that the workers would otherwise compete for (drive letters, temporary files,
offline registry hives, VHD sources, ...) are coordinated between them.

### Credits

* Rufus is Copyright © 2011-2026 [Pete Batard](https://github.com/pbatard) and its many contributors.
  See [rufus.ie](https://rufus.ie) and the About box for the full list of credits and licenses.
* The multi-instance (multi-drive) functionality is by Sam Jackaman ([ExecSam](https://github.com/ExecSam)).
* Like Rufus, Mufus is [Free Software](https://www.gnu.org/philosophy/free-sw), licensed under the
  [GPL v3](https://www.gnu.org/licenses/gpl-3.0) or later.

Please do not report Mufus issues to the Rufus project: use [this repository](https://github.com/ExecSam/Mufus/issues) instead.

Rufus: The Reliable USB Formatting Utility
==========================================

[![VS2022 Build Status](https://img.shields.io/github/actions/workflow/status/pbatard/rufus/vs2026.yml?branch=master&style=flat-square&label=VS2026%20Build)](https://github.com/pbatard/rufus/actions/workflows/vs2026.yml)
[![MinGW Build Status](https://img.shields.io/github/actions/workflow/status/pbatard/rufus/mingw.yml?branch=master&style=flat-square&label=MinGW%20Build)](https://github.com/pbatard/rufus/actions/workflows/mingw.yml)
[![Coverity Scan Status](https://img.shields.io/coverity/scan/2172.svg?style=flat-square&label=Coverity%20Analysis)](https://scan.coverity.com/projects/pbatard-rufus)  
[![Latest Release](https://img.shields.io/github/release-pre/pbatard/rufus.svg?style=flat-square&label=Latest%20Release)](https://github.com/pbatard/rufus/releases)
[![Licence](https://img.shields.io/badge/license-GPLv3-blue.svg?style=flat-square&label=License)](https://www.gnu.org/licenses/gpl-3.0.en.html)
[![Download Stats](https://img.shields.io/github/downloads/pbatard/rufus/total.svg?label=Downloads&style=flat-square)](https://github.com/pbatard/rufus/releases)
[![Contributors](https://img.shields.io/github/contributors/pbatard/rufus.svg?style=flat-square&label=Contributors)](https://github.com/pbatard/rufus/graphs/contributors)

![Rufus logo](https://raw.githubusercontent.com/pbatard/rufus/master/res/icons/rufus-128.png)

Rufus is a utility that helps format and create bootable USB flash drives.

Features
--------

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
* Modern and familiar UI, with [38 languages natively supported](https://github.com/pbatard/rufus/wiki/FAQ#What_languages_are_natively_supported_by_Rufus)
* Small footprint. No installation required.
* Portable. Secure Boot compatible.
* 100% [Free Software](https://www.gnu.org/philosophy/free-sw) ([GPL v3](https://www.gnu.org/licenses/gpl-3.0))

Compilation
-----------

Use either Visual Studio 2026 or MinGW and then invoke the `.sln` or `configure`/`make` respectively.

#### Visual Studio

Rufus is an OSI compliant Open Source project. You are entitled to
download and use the *freely available* [Visual Studio Community Edition](https://www.visualstudio.com/vs/community/)
to build, run or develop for Rufus. As per the Visual Studio Community Edition license,
this applies regardless of whether you are an individual or a corporate user.

Additional information
----------------------

Rufus provides extensive information about what it is doing, either through its
easily accessible log, or through the [Windows debug facility](https://docs.microsoft.com/en-us/sysinternals/downloads/debugview).

* [__Official Website__](https://rufus.ie)
* [FAQ](https://github.com/pbatard/rufus/wiki/FAQ)
* [Security and safety measures](https://github.com/pbatard/rufus/wiki/Security)

Enhancements/Bugs
-----------------

Please use the [GitHub issue tracker](https://github.com/pbatard/rufus/issues)
for reporting problems or suggesting new features.
