<#
  Mufus end-to-end tests.

  Creates VHDX disks, attaches them, and drives the real Mufus UI (through window messages) to write
  to all of them at once, then verifies the content of every disk. Must run elevated (Windows PowerShell 5.1).

  SAFETY: the tests abort before writing anything if Mufus lists any device other than the test VHDs.
#>
param(
	[Parameter(Mandatory = $true)][string]$Exe,
	[Parameter(Mandatory = $true)][string]$Images,
	[Parameter(Mandatory = $true)][string]$WorkDir,
	[int[]]$SizesMB = @(1024, 2048, 3072),
	[string[]]$Tests = @('ui', 'exclusive', 'nonboot', 'freedos', 'dd', 'iso', 'isombr', 'cancel', 'single', 'blank', 'win'),
	# Upstream Rufus executable, for the 'ui' test to take the same screenshots of, for comparison
	[string]$RufusExe,
	# Official Windows ISO for the 'win' test (skipped if not provided), and the disks it uses
	[string]$WinIso,
	[int[]]$WinSizesMB = @(12288, 14336, 16384)
)

$ErrorActionPreference = 'Stop'
# With 'powershell -File', '-Tests a,b' is passed as a single 'a,b' string
$Tests = @($Tests | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
# Have the worker processes timestamp their log
$env:MUFUS_LOG_TIMESTAMPS = '1'
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$Report = Join-Path $WorkDir 'report.txt'
Set-Content -Path $Report -Value "Mufus end-to-end test report - $(Get-Date)" -Encoding UTF8

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

public static class Win {
	public delegate bool EnumProc(IntPtr h, IntPtr l);
	[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
	[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
	[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
	[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
	[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
	[DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
	[DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
	[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
	[DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")]
	public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr res);
	[DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageTimeoutW")]
	public static extern IntPtr SendMessageTimeoutS(IntPtr h, uint m, IntPtr w, StringBuilder l, uint flags, uint timeout, out IntPtr res);

	public static List<IntPtr> TopWindows(uint pid) {
		var list = new List<IntPtr>();
		EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) list.Add(h); return true; }, IntPtr.Zero);
		return list;
	}
	public static string ClassOf(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, sb.Capacity); return sb.ToString(); }
	// SMTO_NORMAL with a long timeout: Mufus may be busy for a few seconds after an operation
	public static IntPtr Send(IntPtr h, uint m, IntPtr w, IntPtr l) { IntPtr r; SendMessageTimeout(h, m, w, l, 0, 60000, out r); return r; }
	public static string Text(IntPtr h) {
		IntPtr len; SendMessageTimeout(h, 0x000E, IntPtr.Zero, IntPtr.Zero, 0, 60000, out len);
		var sb = new StringBuilder((int)len + 1); IntPtr r;
		SendMessageTimeoutS(h, 0x000D, (IntPtr)sb.Capacity, sb, 0, 60000, out r);
		return sb.ToString();
	}
	public static int ComboCount(IntPtr c) { return (int)Send(c, 0x0146, IntPtr.Zero, IntPtr.Zero); }
	public static int ComboCurSel(IntPtr c) { return (int)Send(c, 0x0147, IntPtr.Zero, IntPtr.Zero); }
	public static string ComboItem(IntPtr c, int i) {
		int len = (int)Send(c, 0x0149, (IntPtr)i, IntPtr.Zero);
		var sb = new StringBuilder(Math.Max(len, 0) + 2); IntPtr r;
		SendMessageTimeoutS(c, 0x0148, (IntPtr)i, sb, 0, 60000, out r);
		return sb.ToString();
	}
	public static string HashPhysical(int disk, long length) {
		using (var h = new Microsoft.Win32.SafeHandles.SafeFileHandle(CreateFile(@"\\.\PhysicalDrive" + disk, 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero), true))
		using (var fs = new FileStream(h, FileAccess.Read, 1 << 20))
		using (var sha = SHA256.Create()) {
			var buf = new byte[1 << 20]; long left = length;
			while (left > 0) {
				int n = fs.Read(buf, 0, (int)Math.Min(buf.Length, left));
				if (n <= 0) throw new IOException("Short read on PhysicalDrive" + disk);
				sha.TransformBlock(buf, 0, n, null, 0); left -= n;
			}
			sha.TransformFinalBlock(buf, 0, 0);
			return BitConverter.ToString(sha.Hash).Replace("-", "").ToLowerInvariant();
		}
	}
	[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
	static extern IntPtr CreateFile(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);

	// Screenshots
	public struct RECT { public int Left, Top, Right, Bottom; }
	[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
	[DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
	[DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
	[DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
	// Copies the window from the screen, i.e. what the user sees
	public static void ScreenShot(IntPtr h, string path) {
		RECT v;
		SetForegroundWindow(h);
		System.Threading.Thread.Sleep(500);
		// The visible bounds, without the invisible borders that GetWindowRect() includes
		if (DwmGetWindowAttribute(h, 9, out v, Marshal.SizeOf(typeof(RECT))) != 0) GetWindowRect(h, out v);	// DWMWA_EXTENDED_FRAME_BOUNDS
		using (var bmp = new System.Drawing.Bitmap(v.Right - v.Left, v.Bottom - v.Top)) {
			using (var g = System.Drawing.Graphics.FromImage(bmp))
				g.CopyFromScreen(v.Left, v.Top, 0, 0, bmp.Size);
			bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
		}
	}
}
'@ -ReferencedAssemblies System.Drawing
# Screenshots must be taken in physical pixels
[void][Win]::SetProcessDPIAware()

# Control IDs (src/resource.h)
$ID = @{
	IDOK = 1; IDCANCEL = 2; IDYES = 6; IDNO = 7
	DEVICE = 1001; FILE_SYSTEM = 1002; START = 1003; LABEL = 1007; QUICK_FORMAT = 1008; BOOT_SELECTION = 1011
	BAD_BLOCKS = 1009; LOG_EDIT = 1055; MULTI_DRIVE = 1200; MULTI_DEVICE = 1202; MUFUS_SELECT_ALL = 1204; MUFUS_SUMMARY = 1208
	MUFUS_LIST = 1203; MUFUS_SELECT_NONE = 1205; MUFUS_COUNT = 1206
	PARTITION_TYPE = 1004; CLUSTER_SIZE = 1005; TARGET_SYSTEM = 1017; IMAGE_OPTION = 1026
	ADVANCED_DRIVE = 1028; ADVANCED_FORMAT = 1029; ABOUT = 1052; SELECTION_CHOICE1 = 1078
}
$WM_COMMAND = 0x0111; $WM_CLOSE = 0x0010; $CB_SETCURSEL = 0x014E; $BM_SETCHECK = 0x00F1
$ProcName = [IO.Path]::GetFileNameWithoutExtension($Exe)

function Log([string]$m) {
	$line = '[{0:HH:mm:ss}] {1}' -f (Get-Date), $m
	Write-Host $line
	Add-Content -Path $Report -Value $line -Encoding UTF8
}
function Fail([string]$m) { throw "FAILED: $m" }

function Wait-Until([scriptblock]$Cond, [int]$TimeoutSec, [string]$What) {
	$end = (Get-Date).AddSeconds($TimeoutSec)
	while ((Get-Date) -lt $end) {
		$r = & $Cond
		if ($r) { return $r }
		Start-Sleep -Milliseconds 250
	}
	Fail "Timeout waiting for $What"
}

function Get-Dialogs($proc) {
	[Win]::TopWindows([uint32]$proc.Id) | Where-Object { [Win]::IsWindowVisible($_) -and ([Win]::ClassOf($_) -eq '#32770') }
}
function Get-MainWindow($proc) {
	Get-Dialogs $proc | Where-Object { [Win]::Text($_) -match '^[MR]ufus \d+\.\d+' } | Select-Object -First 1
}
function Command($hwnd, [int]$id) { [void][Win]::PostMessage($hwnd, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero) }
function Get-DialogText($h) {
	$texts = @()
	[void][Win]::EnumChildWindows($h, { param($c, $l) if ([Win]::ClassOf($c) -eq 'Static') { $script:__t += [Win]::Text($c) }; $true }, [IntPtr]::Zero)
	return ($script:__t -join ' | ')
}

function Select-ComboByText($main, [int]$ctrl, [string]$pattern) {
	$c = [Win]::GetDlgItem($main, $ctrl)
	$n = [Win]::ComboCount($c)
	for ($i = 0; $i -lt $n; $i++) {
		if ([Win]::ComboItem($c, $i) -match $pattern) {
			[void][Win]::Send($c, $CB_SETCURSEL, [IntPtr]$i, [IntPtr]::Zero)
			[void][Win]::Send($main, $WM_COMMAND, [IntPtr](($ctrl) -bor (1 -shl 16)), $c)	# CBN_SELCHANGE
			Start-Sleep -Milliseconds 300
			return [Win]::ComboItem($c, $i)
		}
	}
	Fail "No item matching '$pattern' in control $ctrl"
}

# Windows customization: bypass the TPM/Secure Boot/RAM requirements (which alters boot.wim) and a few
# options that only go into unattend.xml, and nothing that requires a download or erases disks.
$WueWanted = 'TPM 2\.0|online Microsoft account|data collection|BitLocker'
function Set-WueOptions($h) {
	$opts = @()
	for ($i = 0; $i -lt 16; $i++) {
		$c = [Win]::GetDlgItem($h, $ID.SELECTION_CHOICE1 + $i)
		if (($c -eq [IntPtr]::Zero) -or -not [Win]::IsWindowVisible($c)) { continue }
		$text = [Win]::Text($c)
		$on = $text -match $WueWanted
		[void][Win]::Send($c, $BM_SETCHECK, [IntPtr]$(if ($on) { 1 } else { 0 }), [IntPtr]::Zero)
		$opts += "[$(if ($on) { 'x' } else { ' ' })] $text"
	}
	Log "  Windows customization: $($opts -join ' | ')"
}

# Handle the dialogs the application may display while an operation is in progress.
# Returns a list of the dialogs that were seen.
$script:Seen = @()
function Handle-Dialogs($proc, $main) {
	foreach ($h in (Get-Dialogs $proc)) {
		if ($h -eq $main) { continue }
		$title = [Win]::Text($h)
		if ([Win]::GetDlgItem($h, $ID.LOG_EDIT) -ne [IntPtr]::Zero) { continue }	# Log window
		if ([Win]::GetDlgItem($h, $ID.MUFUS_SUMMARY) -ne [IntPtr]::Zero) { continue }	# Multi-drive status window
		$script:__t = @(); $text = Get-DialogText $h
		$key = "$h|$title"
		if ($script:Seen -notcontains $key) {
			$script:Seen += $key
			Log "  Dialog: '$title' - $text"
		}
		switch -Regex ($title) {
			'Windows User Experience' { Set-WueOptions $h; Command $h $ID.IDOK; continue }
			'Multi-drive mode$' { Command $h $ID.IDYES; continue }		# Destruction confirmation (OK)
			'^(Mufus|Rufus)$' { Command $h $ID.IDYES; continue }		# Single drive destruction confirmation (OK)
			'ISOHybrid|ESP' { Command $h $ID.IDOK; continue }		# ISO mode selection => first choice
			'Multiple partitions|partitions' { Command $h $ID.IDYES; continue }
			'Cancellation$' { Command $h $ID.IDYES; continue }		# Confirm the cancellation request
			'^Cancelled$' { Command $h $ID.IDNO; continue }			# "Operation cancelled" information
			'drives failed|Error|Failed' { $script:Errors += "$title - $text"; Command $h $ID.IDNO; continue }
			default { Log "  Unexpected dialog '$title' => closing it"; $script:Errors += "Unexpected dialog: $title - $text"; Command $h $ID.IDNO; Command $h $ID.IDCANCEL }
		}
	}
}

function Start-Mufus([string]$image, [string]$exe = $Exe) {
	$args = @('-g')
	if ($image) { $args += @('-i', "`"$image`"") }
	$p = Start-Process -FilePath $exe -ArgumentList $args -PassThru
	$main = Wait-Until { Get-MainWindow $p } 60 'main window'
	# Wait for the device list to be populated and for any image scan to complete
	Wait-Until { [Win]::ComboCount([Win]::GetDlgItem($main, $ID.DEVICE)) -ge 1 } 60 'device enumeration' | Out-Null
	if ($image) {
		Wait-Until { [Win]::IsWindowEnabled([Win]::GetDlgItem($main, $ID.START)) } 300 'image scan' | Out-Null
	}
	Start-Sleep -Seconds 1
	return @{ Proc = $p; Main = $main }
}

function Assert-OnlyTestDevices($main) {
	$c = [Win]::GetDlgItem($main, $ID.DEVICE)
	$n = [Win]::ComboCount($c)
	$items = 0..($n - 1) | ForEach-Object { [Win]::ComboItem($c, $_) }
	Log "  Devices listed: $($items -join ' || ')"
	# Mufus never lists the system disk, so if the only other disks are our VHDs, then it can only list those
	$others = @(Get-Disk | Where-Object { -not $_.IsBoot -and -not $_.IsSystem -and ($script:Vhds.Disk -notcontains $_.Number) })
	if ($others.Count -ne 0) { Fail "Non-test disks are present ($($others.FriendlyName -join ', ')) - refusing to continue" }
	if ($n -ne $script:Vhds.Count) { Fail "Expected $($script:Vhds.Count) devices, found $n - refusing to continue" }
}

function Enable-MultiMode($proc, $main) {
	Command $main $ID.MULTI_DRIVE
	$picker = Wait-Until { Get-Dialogs $proc | Where-Object { [Win]::Text($_) -match 'Select target drives' } | Select-Object -First 1 } 15 'drive picker'
	[void][Win]::Send($picker, $WM_COMMAND, [IntPtr]$ID.MUFUS_SELECT_ALL, [IntPtr]::Zero)
	Start-Sleep -Milliseconds 300
	Command $picker $ID.IDOK
	Wait-Until { -not (Get-Dialogs $proc | Where-Object { [Win]::Text($_) -match 'Select target drives' }) } 15 'drive picker to close' | Out-Null
	$multi = [Win]::GetDlgItem($main, $ID.MULTI_DEVICE)
	$sel = Wait-Until { $t = [Win]::ComboItem($multi, 0); if ($t -match 'drives selected') { $t } } 10 'multi-drive selector'
	Log "  Multi-drive selector shows: '$sel'"
	if (-not [Win]::IsWindowVisible($multi)) { Fail 'Multi-drive selector is not visible' }
	if ([Win]::IsWindowVisible([Win]::GetDlgItem($main, $ID.DEVICE))) { Fail 'Single device dropdown is still visible in multi-drive mode' }
}

function Run-Operation($proc, $main, [int]$TimeoutSec = 900, [int]$CancelAfterSec = 0, [scriptblock]$OnRunning = $null) {
	$script:Errors = @(); $script:Seen = @()
	$start = [Win]::GetDlgItem($main, $ID.START)
	$t0 = Get-Date
	Command $main $ID.START
	$maxWorkers = 0; $started = $false; $cancelled = $false; $tWorkers = $null
	$end = $t0.AddSeconds($TimeoutSec)
	while ((Get-Date) -lt $end) {
		Handle-Dialogs $proc $main
		$workers = @(Get-CimInstance Win32_Process -Filter "Name='$ProcName.exe'" | Where-Object { $_.CommandLine -match '--mufus-worker' }).Count
		if ($workers -gt $maxWorkers) { $maxWorkers = $workers }
		# Called once, a few seconds after all the workers have started
		if (($workers -gt 0) -and ($null -eq $tWorkers)) { $tWorkers = Get-Date }
		if ($OnRunning -and $tWorkers -and (((Get-Date) - $tWorkers).TotalSeconds -ge 4)) { & $OnRunning; $OnRunning = $null }
		$enabled = [Win]::IsWindowEnabled($start)
		if (-not $enabled) { $started = $true }
		if ($started -and ($CancelAfterSec -gt 0) -and -not $cancelled -and ((Get-Date) - $t0).TotalSeconds -ge $CancelAfterSec) {
			Log "  Requesting cancellation"
			Command $main $ID.IDCANCEL
			$cancelled = $true
		}
		if ($started -and $enabled) { break }
		Start-Sleep -Milliseconds 400
	}
	if ((Get-Date) -ge $end) { Fail "Operation timeout after $TimeoutSec s" }
	# Process any final dialog (results, cancellation, ...)
	Start-Sleep -Seconds 2
	Handle-Dialogs $proc $main
	Start-Sleep -Seconds 1
	Handle-Dialogs $proc $main
	$elapsed = [int]((Get-Date) - $t0).TotalSeconds
	$left = @(Get-CimInstance Win32_Process -Filter "Name='$ProcName.exe'" | Where-Object { $_.CommandLine -match '--mufus-worker' }).Count
	return @{ MaxWorkers = $maxWorkers; Elapsed = $elapsed; Errors = $script:Errors; WorkersLeft = $left }
}

function Get-StatusWindow($proc) {
	foreach ($h in [Win]::TopWindows([uint32]$proc.Id)) {
		if ([Win]::GetDlgItem($h, $ID.MUFUS_SUMMARY) -ne [IntPtr]::Zero) { return $h }
	}
	return $null
}
function Get-StatusSummary($proc) {
	$h = Get-StatusWindow $proc
	if ($h) { return [Win]::Text([Win]::GetDlgItem($h, $ID.MUFUS_SUMMARY)) }
	return $null
}

$Shots = Join-Path $WorkDir 'shots'
# NB: PrintWindow() was found to omit some child controls, so screenshots are copied from the screen,
# after bringing the window to the foreground
function Shot($hwnd, [string]$name) {
	if (-not $hwnd -or ($hwnd -eq [IntPtr]::Zero)) { Log "  (no window for screenshot '$name')"; return }
	New-Item -ItemType Directory -Force -Path $Shots | Out-Null
	Start-Sleep -Milliseconds 500
	try { [Win]::ScreenShot($hwnd, (Join-Path $Shots "$name.png")) } catch { Log "  Screenshot '$name' failed: $_" }
}
# Portable copy of an application (settings in an .ini file rather than the registry), with update
# checks disabled and the given dark mode setting (1 = dark, 2 = light). Returns the copy's path.
function Get-PortableCopy([string]$name, [string]$exe, [int]$darkMode) {
	$dir = Join-Path $WorkDir "portable-$name"
	New-Item -ItemType Directory -Force -Path $dir | Out-Null
	$copy = Join-Path $dir ([IO.Path]::GetFileName($exe))
	Copy-Item $exe $copy -Force
	Set-Content -Path (Join-Path $dir "$name.ini") -Value "DarkMode = $darkMode`r`nUpdateCheckInterval = -1" -Encoding ASCII
	return $copy
}
function Combo-Text($main, [int]$ctrl) { $c = [Win]::GetDlgItem($main, $ctrl); [Win]::ComboItem($c, [Win]::ComboCurSel($c)) }

function Save-Log($proc, [string]$name) {
	foreach ($h in [Win]::TopWindows([uint32]$proc.Id)) {
		$e = [Win]::GetDlgItem($h, $ID.LOG_EDIT)
		if ($e -ne [IntPtr]::Zero) {
			$path = Join-Path $WorkDir "log-$name.txt"
			Set-Content -Path $path -Value ([Win]::Text($e)) -Encoding UTF8
			return $path
		}
	}
}

function Stop-Mufus($ctx) {
	[void][Win]::PostMessage($ctx.Main, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
	if (-not $ctx.Proc.WaitForExit(20000)) { Log '  Mufus did not exit - killing it'; $ctx.Proc.Kill() }
}

function Get-DiskVolumes([int]$disk) {
	Update-Disk -Number $disk -ErrorAction SilentlyContinue
	@(Get-Partition -DiskNumber $disk -ErrorAction SilentlyContinue | Get-Volume -ErrorAction SilentlyContinue)
}
function Get-DiskLetter([int]$disk) {
	$p = Get-Partition -DiskNumber $disk -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter } | Select-Object -First 1
	if ($p) { return "$($p.DriveLetter):" }
	return $null
}

# ---------------------------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------------------------
Log "Exe: $Exe"
Log "Windows: $([Environment]::OSVersion.VersionString)"
$free = (Get-PSDrive -Name ($WorkDir.Substring(0, 1))).Free
Log ("Free space on work drive: {0:N1} GB" -f ($free / 1GB))

$script:Vhds = @()
function Remove-TestDisks {
	foreach ($v in $script:Vhds) {
		try { Dismount-DiskImage -ImagePath $v.Path | Out-Null; Remove-Item $v.Path -Force } catch { Log "Cleanup of $($v.Path) failed: $_" }
	}
	$script:Vhds = @()
}
# Creates and attaches one VHDX per size, unless the current disks already have these sizes
function Use-TestDisks([int[]]$sizes) {
	if ((@($script:Vhds.Size) -join ',') -eq ($sizes -join ',')) { return }
	Remove-TestDisks
	$i = 0
	foreach ($size in $sizes) {
		$i++
		$path = Join-Path $WorkDir "mufus-test-$i.vhdx"
		if (Test-Path $path) {
			try { Dismount-DiskImage -ImagePath $path -ErrorAction SilentlyContinue | Out-Null } catch {}
			Remove-Item $path -Force
		}
		$dp = Join-Path $WorkDir 'diskpart.txt'
		Set-Content -Path $dp -Value "create vdisk file=`"$path`" maximum=$size type=expandable" -Encoding ASCII
		diskpart /s $dp | Out-Null
		Mount-DiskImage -ImagePath $path -NoDriveLetter | Out-Null
		$disk = Wait-Until { Get-Disk | Where-Object { $_.Location -eq $path } } 30 "disk for $path"
		Log "Attached $path ($size MB) as disk $($disk.Number)"
		$script:Vhds += [pscustomobject]@{ Path = $path; Size = $size; Disk = $disk.Number }
	}
	# Diagnostic: how long VDS takes to answer once the new disks are attached
	$sw = [Diagnostics.Stopwatch]::StartNew()
	'list disk' | diskpart | Out-Null
	Log ("VDS listed the disks in {0:N1} s" -f $sw.Elapsed.TotalSeconds)
	$others = Get-Disk | Where-Object { -not $_.IsBoot -and -not $_.IsSystem -and ($script:Vhds.Disk -notcontains $_.Number) }
	if ($others) { Log "WARNING: other non-system disks are present: $($others | ForEach-Object { "$($_.Number) $($_.FriendlyName)" })" }
}

$results = [ordered]@{}
function Record([string]$test, [bool]$ok, [string]$details) {
	$results[$test] = @{ Ok = $ok; Details = $details }
	Log ("RESULT {0}: {1} {2}" -f $test, $(if ($ok) { 'PASS' } else { 'FAIL' }), $details)
}

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
$Known = 'ui', 'exclusive', 'nonboot', 'freedos', 'dd', 'iso', 'isombr', 'cancel', 'single', 'blank', 'win'
foreach ($test in $Tests) {
	if ($Known -notcontains $test) { Record $test $false "unknown test (known tests: $($Known -join ', '))"; continue }
	if (($test -eq 'win') -and -not $WinIso) { Log "=== Test: win - skipped (no -WinIso) ==="; continue }
	Log "=== Test: $test ==="
	$ctx = $null
	try {
		Use-TestDisks $(if ($test -eq 'win') { $WinSizesMB } else { $SizesMB })
		switch ($test) {
			{ $_ -in 'nonboot', 'blank' } {
				if ($test -eq 'blank') {
					foreach ($v in $script:Vhds) {
						# Clear-Disk refuses disks that are already blank (e.g. after the cancel test)
						if ((Get-Disk -Number $v.Disk).PartitionStyle -ne 'RAW') { Clear-Disk -Number $v.Disk -RemoveData -RemoveOEM -Confirm:$false }
					}
					Log "  Wiped disks $($script:Vhds.Disk -join ', ') (now: $((Get-Disk -Number $script:Vhds.Disk).PartitionStyle -join ', '))"
					Start-Sleep -Seconds 5
				}
				$ctx = Start-Mufus $null
				Assert-OnlyTestDevices $ctx.Main
				Log ("  Boot selection: " + (Select-ComboByText $ctx.Main $ID.BOOT_SELECTION 'Non bootable|Non-bootable'))
				Log ("  File system: " + (Select-ComboByText $ctx.Main $ID.FILE_SYSTEM '^FAT32|^Large FAT32'))
				Enable-MultiMode $ctx.Proc $ctx.Main
				$r = Run-Operation $ctx.Proc $ctx.Main
				$summary = Get-StatusSummary $ctx.Proc
				Log "  Status: '$summary', max concurrent workers: $($r.MaxWorkers), time: $($r.Elapsed) s"
				$labels = @()
				$problems = @()
				foreach ($v in $script:Vhds) {
					$vol = Get-DiskVolumes $v.Disk | Select-Object -First 1
					if (-not $vol) { $problems += "disk $($v.Disk): no volume"; continue }
					Log "  Disk $($v.Disk): FS=$($vol.FileSystem) Label='$($vol.FileSystemLabel)' Size=$([int]($vol.Size / 1MB)) MB"
					if ($vol.FileSystem -ne 'FAT32') { $problems += "disk $($v.Disk): FS is $($vol.FileSystem)" }
					$labels += $vol.FileSystemLabel
				}
				if (($labels | Select-Object -Unique).Count -ne $labels.Count) { $problems += "labels are not drive specific: $($labels -join ', ')" }
				if ($r.MaxWorkers -lt $script:Vhds.Count) { $problems += "only $($r.MaxWorkers) concurrent workers" }
				if ($r.Errors.Count) { $problems += $r.Errors }
				if ($summary -notmatch "$($script:Vhds.Count) of $($script:Vhds.Count) drives written successfully") { $problems += "summary: $summary" }
				Record $test ($problems.Count -eq 0) ($problems -join '; ')
			}
			'freedos' {
				$ctx = Start-Mufus $null
				Assert-OnlyTestDevices $ctx.Main
				Log ("  Boot selection: " + (Select-ComboByText $ctx.Main $ID.BOOT_SELECTION 'FreeDOS'))
				Enable-MultiMode $ctx.Proc $ctx.Main
				$r = Run-Operation $ctx.Proc $ctx.Main
				$summary = Get-StatusSummary $ctx.Proc
				Log "  Status: '$summary', max concurrent workers: $($r.MaxWorkers), time: $($r.Elapsed) s"
				$problems = @()
				foreach ($v in $script:Vhds) {
					$l = Get-DiskLetter $v.Disk
					if (-not $l) { $problems += "disk $($v.Disk): no drive letter"; continue }
					foreach ($f in 'KERNEL.SYS', 'COMMAND.COM') {
						if (-not (Test-Path "$l\$f")) { $problems += "disk $($v.Disk): $f missing" }
					}
					Log "  Disk $($v.Disk) ($l): $((Get-ChildItem "$l\" -Force | Select-Object -ExpandProperty Name) -join ', ')"
				}
				if ($r.Errors.Count) { $problems += $r.Errors }
				if ($summary -notmatch "$($script:Vhds.Count) of $($script:Vhds.Count) drives written successfully") { $problems += "summary: $summary" }
				Record $test ($problems.Count -eq 0) ($problems -join '; ')
			}
			'dd' {
				$img = Join-Path $Images 'dd.img'
				$ctx = Start-Mufus $img
				Assert-OnlyTestDevices $ctx.Main
				Enable-MultiMode $ctx.Proc $ctx.Main
				$r = Run-Operation $ctx.Proc $ctx.Main
				$summary = Get-StatusSummary $ctx.Proc
				Log "  Status: '$summary', max concurrent workers: $($r.MaxWorkers), time: $($r.Elapsed) s"
				Stop-Mufus $ctx; $ctx = $null
				$len = (Get-Item $img).Length
				$expected = (Get-FileHash -Algorithm SHA256 $img).Hash.ToLower()
				$problems = @()
				foreach ($v in $script:Vhds) {
					$h = [Win]::HashPhysical($v.Disk, $len)
					Log "  Disk $($v.Disk): first $len bytes SHA-256 $h"
					if ($h -ne $expected) { $problems += "disk $($v.Disk): content mismatch" }
				}
				if ($r.Errors.Count) { $problems += $r.Errors }
				if ($summary -notmatch "$($script:Vhds.Count) of $($script:Vhds.Count) drives written successfully") { $problems += "summary: $summary" }
				Record $test ($problems.Count -eq 0) ($problems -join '; ')
			}
			{ $_ -in 'iso', 'isombr' } {
				$img = Join-Path $Images 'efi.iso'
				$ctx = Start-Mufus $img
				Assert-OnlyTestDevices $ctx.Main
				if ($test -eq 'isombr') {
					# MBR for a UEFI target => Rufus uses a constant "UEFI" marker as the disk signature, which
					# Mufus must not use on fixed disks (such as VHDs), since Windows takes them offline on collision
					Log ("  Partition scheme: " + (Select-ComboByText $ctx.Main $ID.PARTITION_TYPE '^MBR'))
					Log ("  Target system: " + (Combo-Text $ctx.Main $ID.TARGET_SYSTEM))
				}
				Enable-MultiMode $ctx.Proc $ctx.Main
				$r = Run-Operation $ctx.Proc $ctx.Main
				$summary = Get-StatusSummary $ctx.Proc
				Log "  Status: '$summary', max concurrent workers: $($r.MaxWorkers), time: $($r.Elapsed) s"
				$manifest = Get-Content (Join-Path $Images 'efi.iso.manifest') | ForEach-Object { $a = $_ -split "`t"; @{ Path = $a[0]; Hash = $a[1] } }
				$problems = @()
				foreach ($v in $script:Vhds) {
					$l = Get-DiskLetter $v.Disk
					if (-not $l) { $problems += "disk $($v.Disk): no drive letter"; continue }
					$bad = 0
					foreach ($m in $manifest) {
						$f = Join-Path "$l\" ($m.Path -replace '/', '\')
						if (-not (Test-Path $f) -or ((Get-FileHash -Algorithm SHA256 $f).Hash.ToLower() -ne $m.Hash)) { $bad++ }
					}
					$vol = Get-DiskVolumes $v.Disk | Where-Object { $_.DriveLetter } | Select-Object -First 1
					Log "  Disk $($v.Disk) ($l): FS=$($vol.FileSystem) Label='$($vol.FileSystemLabel)' - $($manifest.Count - $bad)/$($manifest.Count) files verified"
					if ($bad) { $problems += "disk $($v.Disk): $bad files missing or corrupted" }
				}
				$disks = @(Get-Disk -Number $script:Vhds.Disk)
				Log "  Disks: $(($disks | ForEach-Object { '{0} {1} 0x{2:X8} {3}' -f $_.Number, $_.PartitionStyle, [uint32]$_.Signature, $_.OperationalStatus }) -join ', ')"
				$offline = @($disks | Where-Object { $_.IsOffline })
				if ($offline.Count) { $problems += "disks offline: $(($offline | ForEach-Object { "$($_.Number) ($($_.OfflineReason))" }) -join ', ')" }
				$mbr = @($disks | Where-Object { $_.PartitionStyle -eq 'MBR' })
				if (($mbr.Signature | Select-Object -Unique).Count -ne $mbr.Count) { $problems += 'duplicate disk signatures' }
				if (($test -eq 'isombr') -and ($mbr.Count -ne $disks.Count)) { $problems += 'not all disks are MBR' }
				if ($r.Errors.Count) { $problems += $r.Errors }
				if ($summary -notmatch "$($script:Vhds.Count) of $($script:Vhds.Count) drives written successfully") { $problems += "summary: $summary" }
				Record $test ($problems.Count -eq 0) ($problems -join '; ')
			}
			'cancel' {
				$ctx = Start-Mufus $null
				Assert-OnlyTestDevices $ctx.Main
				Log ("  Boot selection: " + (Select-ComboByText $ctx.Main $ID.BOOT_SELECTION 'Non bootable|Non-bootable'))
				# A bad blocks check takes long enough for us to cancel it
				[void][Win]::Send([Win]::GetDlgItem($ctx.Main, $ID.BAD_BLOCKS), $BM_SETCHECK, [IntPtr]1, [IntPtr]::Zero)
				Enable-MultiMode $ctx.Proc $ctx.Main
				$r = Run-Operation $ctx.Proc $ctx.Main 600 12
				$summary = Get-StatusSummary $ctx.Proc
				Log "  Status: '$summary', max concurrent workers: $($r.MaxWorkers), time: $($r.Elapsed) s, workers left: $($r.WorkersLeft)"
				$problems = @()
				if ($r.WorkersLeft -ne 0) { $problems += "$($r.WorkersLeft) workers still running" }
				if ($r.MaxWorkers -lt $script:Vhds.Count) { $problems += "only $($r.MaxWorkers) concurrent workers" }
				if ($r.Elapsed -gt 300) { $problems += "cancellation took $($r.Elapsed) s" }
				$problems += ($r.Errors | Where-Object { $_ -notmatch 'Cancel' })
				if (-not [Win]::IsWindowEnabled([Win]::GetDlgItem($ctx.Main, $ID.START))) { $problems += 'UI not re-enabled' }
				Record $test ($problems.Count -eq 0) ($problems -join '; ')
			}
			'single' {
				$ctx = Start-Mufus $null
				Assert-OnlyTestDevices $ctx.Main
				Log ("  Boot selection: " + (Select-ComboByText $ctx.Main $ID.BOOT_SELECTION 'Non bootable|Non-bootable'))
				Log ("  File system: " + (Select-ComboByText $ctx.Main $ID.FILE_SYSTEM '^NTFS'))
				$dev = [Win]::GetDlgItem($ctx.Main, $ID.DEVICE)
				$target = [Win]::ComboItem($dev, [Win]::ComboCurSel($dev))
				Log "  Single target: $target"
				$before = @{}
				foreach ($v in $script:Vhds) { $before[$v.Disk] = (Get-DiskVolumes $v.Disk | Select-Object -First 1).FileSystem }
				$r = Run-Operation $ctx.Proc $ctx.Main
				Log "  Max concurrent workers: $($r.MaxWorkers), time: $($r.Elapsed) s"
				$problems = @()
				$ntfs = @()
				foreach ($v in $script:Vhds) {
					$fs = (Get-DiskVolumes $v.Disk | Select-Object -First 1).FileSystem
					Log "  Disk $($v.Disk): FS=$fs (before: $($before[$v.Disk]))"
					if (($fs -eq 'NTFS') -and ($before[$v.Disk] -ne 'NTFS')) { $ntfs += $v.Disk }
				}
				if ($ntfs.Count -ne 1) { $problems += "expected exactly 1 drive to be reformatted to NTFS, got $($ntfs.Count)" }
				if ($r.MaxWorkers -ne 0) { $problems += 'worker processes were used in single drive mode' }
				if ($r.Errors.Count) { $problems += $r.Errors }
				Record $test ($problems.Count -eq 0) ($problems -join '; ')
			}
			'exclusive' {
				# Rufus and Mufus alter the same system settings, so each must refuse to start while the other runs
				if (-not $RufusExe) { Fail 'this test requires -RufusExe' }
				$problems = @()
				$rufus = Get-PortableCopy 'rufus' $RufusExe 2
				$mufus = Get-PortableCopy 'mufus' $Exe 2
				foreach ($order in @(@($rufus, $mufus), @($mufus, $rufus))) {
					$names = $order | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) }
					$first = $null; $second = $null
					try {
						$first = Start-Process -FilePath $order[0] -ArgumentList '-g' -PassThru
						$firstMain = Wait-Until { Get-MainWindow $first } 60 "$($names[0]) main window"
						$second = Start-Process -FilePath $order[1] -ArgumentList '-g' -PassThru
						$box = Wait-Until { [Win]::TopWindows([uint32]$second.Id) | Where-Object { [Win]::IsWindowVisible($_) -and ([Win]::Text($_) -eq 'Other instance detected') } | Select-Object -First 1 } 30 "$($names[1]) to report $($names[0])"
						$script:__t = @()
						Log "  $($names[1]) started after $($names[0]): '$(Get-DialogText $box)'"
						[void][Win]::PostMessage($box, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
						if (-not $second.WaitForExit(15000)) { $problems += "$($names[1]) did not exit" }
						[void][Win]::PostMessage($firstMain, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
						[void]$first.WaitForExit(20000)
					} finally {
						# Don't leave anything running that would prevent the next tests from starting Mufus
						foreach ($p in @($second, $first)) { if ($p -and -not $p.HasExited) { $p.Kill(); [void]$p.WaitForExit(5000) } }
					}
					Start-Sleep -Seconds 2
				}
				Record $test ($problems.Count -eq 0) ($problems -join '; ')
			}
			'ui' {
				# The same screenshots of upstream Rufus and of Mufus, in light and dark mode, for comparison.
				# Portable copies (with an .ini file) are used, to force the theme without altering any settings.
				$apps = @()
				if ($RufusExe) { $apps += @{ Name = 'rufus'; Exe = $RufusExe } }
				$apps += @{ Name = 'mufus'; Exe = $Exe }
				$problems = @()
				foreach ($theme in @(@{ Name = 'light'; Mode = 2 }, @{ Name = 'dark'; Mode = 1 })) {
					foreach ($app in $apps) {
						# NB: PowerShell variables are case insensitive, so this must not be named $exe (= $Exe)
						$appExe = Get-PortableCopy $app.Name $app.Exe $theme.Mode
						$p = "$($theme.Name)-$($app.Name)"
						$ctx = Start-Mufus $null $appExe
						Assert-OnlyTestDevices $ctx.Main
						Start-Sleep -Seconds 2
						Shot $ctx.Main "$p-1-main"
						Command $ctx.Main $ID.ADVANCED_DRIVE; Command $ctx.Main $ID.ADVANCED_FORMAT
						Start-Sleep -Seconds 1
						Shot $ctx.Main "$p-2-advanced"
						Command $ctx.Main $ID.ADVANCED_DRIVE; Command $ctx.Main $ID.ADVANCED_FORMAT
						Command $ctx.Main $ID.ABOUT
						$about = Wait-Until { Get-Dialogs $ctx.Proc | Where-Object { [Win]::Text($_) -match '^About' } | Select-Object -First 1 } 15 'About dialog'
						Shot $about "$p-3-about"
						Command $about $ID.IDOK
						Wait-Until { -not (Get-Dialogs $ctx.Proc | Where-Object { [Win]::Text($_) -match '^About' }) } 15 'About dialog to close' | Out-Null
						if ($app.Name -eq 'mufus') {
							Log ("  Boot selection: " + (Select-ComboByText $ctx.Main $ID.BOOT_SELECTION 'Non bootable|Non-bootable'))
							Command $ctx.Main $ID.MULTI_DRIVE
							$picker = Wait-Until { Get-Dialogs $ctx.Proc | Where-Object { [Win]::Text($_) -match 'Select target drives' } | Select-Object -First 1 } 15 'drive picker'
							[void][Win]::Send($picker, $WM_COMMAND, [IntPtr]$ID.MUFUS_SELECT_ALL, [IntPtr]::Zero)
							# What the picker actually contains, independently of how it gets captured
							$list = [Win]::GetDlgItem($picker, $ID.MUFUS_LIST)
							$state = @(
								"items=$([int][Win]::Send($list, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero))",	# LVM_GETITEMCOUNT
								"count='$([Win]::Text([Win]::GetDlgItem($picker, $ID.MUFUS_COUNT)))'"
							)
							foreach ($b in @(@('SelectAll', $ID.MUFUS_SELECT_ALL), @('Clear', $ID.MUFUS_SELECT_NONE), @('OK', $ID.IDOK), @('Cancel', $ID.IDCANCEL))) {
								$hb = [Win]::GetDlgItem($picker, $b[1])
								$state += "$($b[0])=$(if ($hb -eq [IntPtr]::Zero) { 'missing' } elseif ([Win]::IsWindowVisible($hb)) { "'$([Win]::Text($hb))'" } else { 'hidden' })"
							}
							Log "  Picker: $($state -join ', ')"
							Shot $picker "$p-4-picker"
							Command $picker $ID.IDOK
							Wait-Until { -not (Get-Dialogs $ctx.Proc | Where-Object { [Win]::Text($_) -match 'Select target drives' }) } 15 'drive picker to close' | Out-Null
							Shot $ctx.Main "$p-5-multi"
							$c = $ctx
							$r = Run-Operation $ctx.Proc $ctx.Main -OnRunning { Shot $c.Main "$p-6-running"; Shot (Get-StatusWindow $c.Proc) "$p-7-status-running" }
							Shot $ctx.Main "$p-8-done"
							Shot (Get-StatusWindow $ctx.Proc) "$p-9-status-done"
							if ($r.Errors.Count) { $problems += $r.Errors }
						}
						Stop-Mufus $ctx; $ctx = $null
					}
				}
				Log "  Screenshots saved to $Shots"
				Record $test ($problems.Count -eq 0) ($problems -join '; ')
			}
			'win' {
				$free = (Get-PSDrive -Name ($WorkDir.Substring(0, 1))).Free
				$need = 3.5 * (Get-Item $WinIso).Length
				if ($free -lt $need) { Fail ("not enough free space for the VHDX disks ({0:N1} GB free, {1:N1} GB needed)" -f ($free / 1GB), ($need / 1GB)) }
				$ctx = Start-Mufus $WinIso
				Assert-OnlyTestDevices $ctx.Main
				foreach ($n in 'BOOT_SELECTION', 'IMAGE_OPTION', 'PARTITION_TYPE', 'TARGET_SYSTEM', 'FILE_SYSTEM', 'CLUSTER_SIZE') {
					Log ("  {0}: {1}" -f $n, (Combo-Text $ctx.Main $ID[$n]))
				}
				Log "  Label: $([Win]::Text([Win]::GetDlgItem($ctx.Main, $ID.LABEL)))"
				Enable-MultiMode $ctx.Proc $ctx.Main
				$c = $ctx
				$r = Run-Operation $ctx.Proc $ctx.Main 5400 0 { Shot $c.Main 'win-running'; Shot (Get-StatusWindow $c.Proc) 'win-status-running' }
				$summary = Get-StatusSummary $ctx.Proc
				Shot $ctx.Main 'win-done'
				Shot (Get-StatusWindow $ctx.Proc) 'win-status-done'
				Log "  Status: '$summary', max concurrent workers: $($r.MaxWorkers), time: $($r.Elapsed) s"
				$problems = @()
				if ($r.Errors.Count) { $problems += $r.Errors }
				if ($summary -notmatch "$($script:Vhds.Count) of $($script:Vhds.Count) drives written successfully") { $problems += "summary: $summary" }
				if ($r.MaxWorkers -lt $script:Vhds.Count) { $problems += "only $($r.MaxWorkers) concurrent workers" }
				$log = Save-Log $ctx.Proc $test
				Log "  Log saved to $log"
				Stop-Mufus $ctx; $ctx = $null

				# Reference content, from the ISO itself
				$sw = [Diagnostics.Stopwatch]::StartNew()
				$iso = Mount-DiskImage -ImagePath $WinIso -Access ReadOnly -PassThru
				try {
					$isoRoot = "$((Get-Volume -DiskImage $iso).DriveLetter):\"
					$ref = @{}
					foreach ($f in Get-ChildItem $isoRoot -Recurse -File -Force) {
						$ref[$f.FullName.Substring($isoRoot.Length)] = @{ Length = $f.Length; Hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash }
					}
				} finally {
					Dismount-DiskImage -ImagePath $WinIso | Out-Null
				}
				Log ("  ISO: {0} files, hashed in {1:N0} s" -f $ref.Count, $sw.Elapsed.TotalSeconds)

				# What Mufus (Rufus) is expected to change, for the selected Windows customization options:
				# - the TPM/Secure Boot/RAM bypass is added to the registry of boot.wim (index 2), and
				#   appraiserres.dll is replaced with an empty file (for in-place upgrades)
				# - setup.exe is replaced with a wrapper (Windows 11 24H2 and later), the original becoming setup.dll
				# - the remaining options go into an unattend.xml, copied to Windows by setup
				$modified = @('sources\boot.wim', 'sources\appraiserres.dll', 'setup.exe')
				$added = @{ 'sources\appraiserres.bak' = 'sources\appraiserres.dll'; 'setup.dll' = 'setup.exe'; 'sources\$OEM$\$$\Panther\unattend.xml' = '' }
				foreach ($v in $script:Vhds) {
					$sw = [Diagnostics.Stopwatch]::StartNew()
					$disk = Get-Disk -Number $v.Disk
					$parts = @(Get-Partition -DiskNumber $v.Disk | ForEach-Object { '#{0} {1:N0} MB {2}' -f $_.PartitionNumber, ($_.Size / 1MB), $(if ($_.DriveLetter) { "$($_.DriveLetter):" }) })
					$vols = @(Get-DiskVolumes $v.Disk | ForEach-Object { "$($_.FileSystem) '$($_.FileSystemLabel)'" })
					Log "  Disk $($v.Disk): $($disk.PartitionStyle) - partitions: $($parts -join ', ') - volumes: $($vols -join ', ')"
					$l = Get-DiskLetter $v.Disk
					if (-not $l) { $problems += "disk $($v.Disk): no drive letter"; continue }
					$root = "$l\"
					$found = @{}; $diffs = @()
					foreach ($f in Get-ChildItem $root -Recurse -File -Force -ErrorAction SilentlyContinue) {
						$rel = $f.FullName.Substring($root.Length)
						if ($rel -match '^(System Volume Information|\$RECYCLE\.BIN)\\') { continue }
						$found[$rel] = $true
						if ($ref.ContainsKey($rel)) {
							if ($modified -contains $rel) { continue }
							if (($f.Length -ne $ref[$rel].Length) -or ((Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash -ne $ref[$rel].Hash)) { $diffs += "differs: $rel" }
						} elseif ($added.ContainsKey($rel)) {
							$src = $added[$rel]
							if ($src -and ((Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash -ne $ref[$src].Hash)) { $diffs += "$rel is not the original $src" }
						} else {
							$diffs += "unexpected: $rel"
						}
					}
					foreach ($k in $ref.Keys) { if (-not $found.ContainsKey($k)) { $diffs += "missing: $k" } }
					foreach ($k in $added.Keys) { if (-not $found.ContainsKey($k)) { $diffs += "not added: $k" } }
					if ((Test-Path "$root\sources\appraiserres.dll") -and ((Get-Item "$root\sources\appraiserres.dll").Length -ne 0)) { $diffs += 'appraiserres.dll was not replaced' }
					if ((Test-Path "$root\setup.exe") -and ((Get-FileHash -Algorithm SHA256 "$root\setup.exe").Hash -eq $ref['setup.exe'].Hash)) { $diffs += 'setup.exe was not replaced' }
					try { [void][xml](Get-Content -Raw -LiteralPath "$root\sources\`$OEM`$\`$`$\Panther\unattend.xml") } catch { $diffs += "unattend.xml is not valid XML: $_" }

					# The bypass must be in the registry of the Windows Setup image of boot.wim
					$mnt = Join-Path $WorkDir 'wim-mount'
					$hive = Join-Path $WorkDir "SYSTEM-$($v.Disk)"
					New-Item -ItemType Directory -Force -Path $mnt | Out-Null
					$lab = ''
					try {
						Mount-WindowsImage -ImagePath "$root\sources\boot.wim" -Index 2 -Path $mnt -ReadOnly | Out-Null
						try { Copy-Item (Join-Path $mnt 'Windows\System32\config\SYSTEM') $hive -Force } finally { Dismount-WindowsImage -Path $mnt -Discard | Out-Null }
						reg load 'HKLM\MUFUS_E2E' $hive | Out-Null
						# (through cmd, as PowerShell 5.1 turns native stderr output into terminating errors)
						try { $lab = (cmd /c 'reg query HKLM\MUFUS_E2E\Setup\LabConfig 2>nul') -join ' ' } finally { reg unload 'HKLM\MUFUS_E2E' | Out-Null }
						Remove-Item $hive -Force -ErrorAction SilentlyContinue
					} catch { $diffs += "boot.wim: $_" }
					foreach ($b in 'BypassTPMCheck', 'BypassSecureBootCheck', 'BypassRAMCheck') {
						if ($lab -notmatch "$b\s+REG_DWORD\s+0x1") { $diffs += "boot.wim: $b missing" }
					}
					Log ("  Disk $($v.Disk) ($l): $($found.Count) files, {0} problems, LabConfig: {1} - checked in {2:N0} s" -f $diffs.Count, $(if ($lab -match 'BypassTPMCheck') { 'present' } else { 'MISSING' }), $sw.Elapsed.TotalSeconds)
					foreach ($d in ($diffs | Select-Object -First 20)) { Log "    $d" }
					if ($diffs.Count) { $problems += "disk $($v.Disk): $($diffs.Count) problems" }
				}
				Record $test ($problems.Count -eq 0) ($problems -join '; ')
			}
		}
	} catch {
		Record $test $false "$($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
	} finally {
		if ($ctx) {
			$log = Save-Log $ctx.Proc $test
			if ($log) { Log "  Log saved to $log" }
			Stop-Mufus $ctx
		}
		Start-Sleep -Seconds 2
	}
}

# ---------------------------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------------------------
Remove-TestDisks
$pass = @($results.Values | Where-Object { $_.Ok }).Count
Log "=== $pass of $($results.Count) tests passed ==="
Set-Content -Path (Join-Path $WorkDir 'done.txt') -Value "$pass/$($results.Count)"
