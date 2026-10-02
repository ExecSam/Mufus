/*
 * Mufus: Multi-instance Rufus
 * Concurrent multi-drive writing support
 * Copyright © 2026 Sam Jackaman (ExecSam)
 *
 * Built on top of Rufus: The Reliable USB Formatting Utility
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <setupapi.h>
#include <winioctl.h>
#include <uxtheme.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "rufus.h"
#include "mufus.h"
#include "drive.h"
#include "format.h"
#include "missing.h"
#include "resource.h"
#include "msapi_utf8.h"
#include "localization.h"
#include "darkmode.h"
#include "vhd.h"
#include "ui.h"
#include "bled/bled.h"
#include "cdio/logging.h"

/* See mufus.h for an overview of how multi-drive writing works */

#define MUFUS_JOB_MAGIC             0x4A46554D	// "MUFJ"
#define MUFUS_JOB_VERSION           1
#define MUFUS_MAX_MSG_SIZE          (16 * MB)
#define MUFUS_PIPE_SIZE             (64 * KB)
#define MUFUS_MAX_CTRLS             48
#define MUFUS_MAX_COMBO_ITEMS       40
#define MUFUS_PATH_SIZE             (4 * MAX_PATH)
#define MUFUS_MAX_LOG_BUFFER        (8 * MB)
// Delay between worker launches, so that the initial partitioning of each drive, which
// goes through Windows' Virtual Disk Service and the mount manager, isn't all hit at once.
#define MUFUS_LAUNCH_STAGGER        750
#define MUFUS_ROW_HEIGHT            24	// In dialog units
#define MUFUS_STATUS_WIDTH          300	// In dialog units

#define _WIDEN(s)                   L ## s
#define WIDEN(s)                    _WIDEN(s)

/*
 * Data sent from the master to a worker
 */
typedef struct {
	int id;
	int kind;
	BOOL enabled;
	int state;
	int cur_sel;
	int nb_items;
	char text[256];
	struct {
		LONG_PTR data;
		char text[128];
	} item[MUFUS_MAX_COMBO_ITEMS];
} mufus_ctrl_t;

enum {
	MUFUS_CTRL_BUTTON = 0,
	MUFUS_CTRL_EDIT,
	MUFUS_CTRL_COMBO,
};

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t struct_size;
	int slot;
	DWORD master_pid;

	// The target drive
	DWORD device_num;
	DWORD drive_index;
	uint32_t drive_port;
	uint64_t drive_size;
	char drive_id[512];
	char drive_name[256];
	char drive_display_name[256];
	char drive_label[128];
	char drive_hub[512];

	// Per-drive adjustments of the options that Rufus derives from the selected drive
	BOOL auto_cluster_size;
	BOOL auto_label;

	// Paths and names
	char image_path[MUFUS_PATH_SIZE];
	char archive_path[MUFUS_PATH_SIZE];
	char unattend_xml_path[MUFUS_PATH_SIZE];
	char vhd_physical_path[128];
	uint64_t vhd_size;
	char hive_name[64];
	char main_title[128];

	// Pre-format state set by the master (see the START flow in rufus.c)
	int boot_type, partition_type, target_type, fs_type, selection_default;
	int default_thread_priority, update_progress_type;
	int unattend_xml_flags, unattend_xml_mask, unattend_edition_index;
	int wintogo_index, wininst_index;
	uint32_t removable_section[2];
	uint8_t image_options;
	BOOL write_as_image, write_as_esp, zero_drive, fast_zeroing, append_silent;
	BOOL lock_drive, enable_ntfs_compression, enable_iso, enable_joliet, enable_rockridge;
	BOOL validate_md5sum, use_vds, enable_file_indexing, force_large_fat32, allow_dual_uefi_bios;
	BOOL use_rufus_mbr, detect_fakes, preserve_timestamps, usb_debug, expert_mode, enable_extra_hashes;
	BOOL advanced_mode_device, advanced_mode_format, ignore_boot_marker, enable_HDDs, enable_VHDs;
	BOOL use_fake_units, mbr_selected_by_user, has_uefi_csm, list_non_usb_removable_drives;
	BOOL is_vds_available;
	BOOL use_own_c32[NB_OLD_C32];
	unsigned long syslinux_ldlinux_len[2];
	uint64_t persistence_size;
	uint64_t total_blocks, extra_blocks;
	uint32_t dur_mins, dur_secs;
	uint16_t rufus_version[3];
	uint16_t embedded_sl_version[2];
	char embedded_sl_version_str[2][12];
	char embedded_sl_version_ext[2][32];
	RUFUS_IMG_REPORT img_report;

	// Snapshot of the main dialog controls
	int nb_ctrls;
	mufus_ctrl_t ctrl[MUFUS_MAX_CTRLS];
	char device_text[256];

	// Variable-length data that follows the structure
	uint32_t grub2_len;
} mufus_job_t;

typedef struct {
	uint32_t type;
	uint32_t size;
} mufus_msg_header_t;

typedef struct {
	int type;
	char* title;
	char* message;
} mufus_prompt_t;

/*
 * Master side state of each drive being written
 */
typedef struct {
	DWORD device_num;
	char id[512];
	char name[256];
	char tag[32];
	char unattend_copy[MAX_PATH];
	mufus_job_t* job;
	size_t job_size;
	HANDLE hProcess;
	HANDLE hReader;
	HANDLE hToWorker;
	HANDLE hFromWorker;
	CRITICAL_SECTION lock;
	CRITICAL_SECTION send_lock;
	// Protected by lock
	char* log;
	size_t log_len, log_max;
	BOOL log_at_line_start;
	int pos;
	char info[256];
	BOOL update_posted;
	BOOL done_received;
	DWORD error_status;
	// Only accessed from the UI thread, or before and after the operation
	BOOL initialized;
	int state;
	int ui_pos;
	char ui_info[256];
	HWND hName, hInfo, hBar;
} mufus_slot_t;

/* Extra globals we need from the rest of the application */
extern BOOL enable_iso, enable_joliet, enable_rockridge, enable_extra_hashes, validate_md5sum;
extern BOOL lock_drive, enable_ntfs_compression, use_vds, enable_file_indexing, force_large_fat32;
extern BOOL use_rufus_mbr, preserve_timestamps, expert_mode, advanced_mode_device, advanced_mode_format;
extern BOOL ignore_boot_marker, enable_HDDs, enable_VHDs, use_fake_units, mbr_selected_by_user;
extern BOOL has_uefi_csm, list_non_usb_removable_drives, zero_drive, fast_zeroing, write_as_image;
extern BOOL write_as_esp, append_silent, is_vds_available, size_check;
extern int selection_default, default_thread_priority, imop_win_sel, update_progress_type;
extern int unattend_xml_flags, unattend_xml_mask, unattend_edition_index, wintogo_index, wininst_index;
extern uint32_t removable_section[2], dur_mins, dur_secs;
extern char *unattend_xml_path, *archive_path;
extern char embedded_sl_version_ext[2][32];
extern unsigned long syslinux_ldlinux_len[2];
extern uint64_t total_blocks, extra_blocks;
extern uint8_t* grub2_buf;
extern long grub2_len;
extern loc_cmd* selected_locale;
extern HANDLE format_thread;
extern HWND hSaveToolbar;
extern RUFUS_DRIVE rufus_drive[MAX_DRIVES];
extern StrArray BlockingProcessList, ImageList;
extern cdio_log_level_t cdio_loglevel_default;
extern const char* FileSystemLabel[FS_MAX];
extern void ComputeClusterSizes(void);
extern void InitProgress(BOOL bOnlyFormat);

/*
 * Globals
 */
BOOL multi_mode = FALSE, mufus_worker = FALSE;
int mufus_nb_selected = 0;
DWORD mufus_selected[MUFUS_MAX_DRIVES];

static char mufus_selected_id[MUFUS_MAX_DRIVES][512], mufus_selected_tag[MUFUS_MAX_DRIVES][32];
static mufus_slot_t slot[MUFUS_MAX_DRIVES];
static int nb_slots = 0;
static BOOL multi_operation = FALSE, vhd_mounted = FALSE;
static uint64_t multi_start_time = 0;
static char vhd_physical_path[128];
static uint64_t vhd_size = 0;
static HWND hMultiDevice = NULL, hMultiDriveToolbar = NULL, hStatusDlg = NULL;
static HIMAGELIST hMultiImageList = NULL;
static WNDPROC multi_device_original_proc = NULL;
static int prompt_queue[4 * MUFUS_MAX_DRIVES], prompt_queue_len = 0;
static mufus_prompt_t* prompt_data[MUFUS_MAX_DRIVES];
static BOOL prompt_showing = FALSE;

/* Drive letters reservation table, shared by the master and its workers */
static HANDLE hLetterMap = NULL, hLetterMutex = NULL;
static uint8_t* letter_owner = NULL;

/* Worker side */
static HANDLE hFromMaster = NULL, hToMaster = NULL, hPromptEvent = NULL;
static CRITICAL_SECTION worker_send_lock, worker_prompt_lock;
static BOOL worker_relay_log = FALSE;
static volatile int prompt_result = IDCANCEL;
static volatile BOOL worker_cancel_requested = FALSE;
static volatile int worker_prompt_type = -1;
static int worker_slot = -1;
static char worker_hive_name[64], worker_vhd_path[128];
static HANDLE hCycleMutex = NULL;
static uint64_t worker_vhd_size = 0;

/* UI strings */
#define STR_MULTI_TOOLTIP           "Write to multiple drives at once (Mufus multi-drive mode)"
#define STR_SELECT_TITLE            "Mufus - Select target drives"
#define STR_SELECT_INFO             "Select up to %d drives to write the current image and options to. " \
                                    "ALL DATA ON THE SELECTED DRIVES WILL BE DESTROYED."
#define STR_SELECT_COUNT            "%d of %d drives selected"
#define STR_SELECT_MAX              "You can select at most %d drives."
#define STR_SELECT_ALL              "Select all"
#define STR_SELECT_NONE             "Clear"
#define STR_STATUS_TITLE            "Mufus - Multi-drive progress"
#define STR_SUMMARY_RUNNING         "Writing %d drives: %d completed"
#define STR_SUMMARY_DONE            "%d of %d drives written successfully"
#define STR_MULTI_SELECTION         "%d drives selected: %s"
#define STR_PROGRESS_TEXT           "Writing %d drives: %0.1f%%"
#define STR_STATE_PENDING           "Waiting..."
#define STR_STATE_STARTING          "Starting..."
#define STR_STATE_SUCCESS           "✓ Done"
#define STR_STATE_CANCELLED         "Cancelled"
#define STR_CONFIRM_TITLE           APPLICATION_NAME " - Multi-drive mode"
#define STR_CONFIRM_TEXT            "WARNING: ALL DATA ON THE FOLLOWING %d DEVICES WILL BE DESTROYED:\n\n%s\n" \
                                    "To continue with this operation, click OK. To quit click CANCEL."
#define STR_DRIVE_GONE              "Drive '%s' is no longer available. Please check your drive selection."
#define STR_DRIVE_FS                "The selected file system (%s) can not be used on drive '%s'."
#define STR_DRIVE_CLUSTER           "The selected cluster size can not be used on drive '%s'.\n\n" \
                                    "Please select the default cluster size instead."
#define STR_DRIVE_PERSISTENCE       "Drive '%s' is too small for the selected persistence size."
#define STR_RESULT_TITLE            "%d of %d drives failed"
#define STR_RESULT_TEXT             "The operation completed with errors:\n\n%s"

/*
 * Pipe I/O
 */
static BOOL PipeWrite(HANDLE h, const void* buf, DWORD size)
{
	const uint8_t* p = (const uint8_t*)buf;
	DWORD written;

	while (size > 0) {
		if (!WriteFile(h, p, size, &written, NULL) || (written == 0))
			return FALSE;
		p += written;
		size -= written;
	}
	return TRUE;
}

static BOOL PipeRead(HANDLE h, void* buf, DWORD size)
{
	uint8_t* p = (uint8_t*)buf;
	DWORD read;

	while (size > 0) {
		if (!ReadFile(h, p, size, &read, NULL) || (read == 0))
			return FALSE;
		p += read;
		size -= read;
	}
	return TRUE;
}

// The handle is only read with the lock held, as the master may close it concurrently
static BOOL SendMsg(HANDLE* h, CRITICAL_SECTION* lock, uint32_t type, const void* data, uint32_t size)
{
	BOOL r = FALSE;
	mufus_msg_header_t hdr = { type, size };

	EnterCriticalSection(lock);
	if ((*h != NULL) && (*h != INVALID_HANDLE_VALUE))
		r = PipeWrite(*h, &hdr, sizeof(hdr)) && ((size == 0) || PipeWrite(*h, data, size));
	LeaveCriticalSection(lock);
	return r;
}

// Returns a malloc'ed, NUL terminated, payload or NULL on error
static uint8_t* RecvMsg(HANDLE h, uint32_t* type, uint32_t* size)
{
	mufus_msg_header_t hdr;
	uint8_t* data;

	if (!PipeRead(h, &hdr, sizeof(hdr)) || (hdr.size > MUFUS_MAX_MSG_SIZE))
		return NULL;
	data = (uint8_t*)malloc(hdr.size + 1);
	if (data == NULL)
		return NULL;
	if ((hdr.size != 0) && !PipeRead(h, data, hdr.size)) {
		free(data);
		return NULL;
	}
	data[hdr.size] = 0;
	*type = hdr.type;
	*size = hdr.size;
	return data;
}

/*
 * Drive letter reservations.
 * Rufus picks the lowest free letter when a drive has none, and frees the letters of the drive it
 * formats until the very end of the process. With multiple drives being written concurrently, this
 * means that two drives could end up being assigned the same letter. To prevent this, the master
 * reserves the existing letters of each target for its worker, and workers record any extra letter
 * they pick, in a table shared by all the processes involved.
 */
static BOOL OpenLetterTable(DWORD master_pid, BOOL create)
{
	char name[64];

	static_sprintf(name, "Local\\Mufus_Letters_%08lX", master_pid);
	hLetterMap = create ? CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 32, name) :
		OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
	if (hLetterMap == NULL)
		return FALSE;
	letter_owner = (uint8_t*)MapViewOfFile(hLetterMap, FILE_MAP_ALL_ACCESS, 0, 0, 32);
	static_sprintf(name, "Local\\Mufus_LettersLock_%08lX", master_pid);
	hLetterMutex = create ? CreateMutexA(NULL, FALSE, name) : OpenMutexA(SYNCHRONIZE, FALSE, name);
	if ((letter_owner == NULL) || (hLetterMutex == NULL)) {
		if (letter_owner != NULL)
			UnmapViewOfFile(letter_owner);
		letter_owner = NULL;
		safe_closehandle(hLetterMap);
		safe_closehandle(hLetterMutex);
		return FALSE;
	}
	return TRUE;
}

static void CloseLetterTable(void)
{
	if (letter_owner != NULL)
		UnmapViewOfFile(letter_owner);
	letter_owner = NULL;
	safe_closehandle(hLetterMap);
	safe_closehandle(hLetterMutex);
}

static void ReleaseLetters(int owner)
{
	int i;

	if ((letter_owner == NULL) || (WaitForSingleObject(hLetterMutex, 5000) == WAIT_TIMEOUT))
		return;
	for (i = 0; i < 26; i++) {
		if (letter_owner[i] == owner + 1)
			letter_owner[i] = 0;
	}
	ReleaseMutex(hLetterMutex);
}

// Replacement for GetUnusedDriveLetter() in workers
char MufusGetUnusedDriveLetter(void)
{
	DWORD drives;
	int i, pass;
	char letter = 0;

	drives = GetLogicalDrives();
	if (letter_owner == NULL) {
		// No coordination possible => same as GetUnusedDriveLetter()
		for (i = 'C' - 'A'; (i <= 'Z' - 'A') && (drives & (1 << i)); i++);
		return (i <= 'Z' - 'A') ? (char)('A' + i) : 0;
	}
	if (WaitForSingleObject(hLetterMutex, 10000) == WAIT_TIMEOUT)
		uprintf("WARNING: Timeout while waiting for the drive letters table");
	// Refresh the list of used letters now that we hold the lock
	drives = GetLogicalDrives();
	// Prefer a free letter we already own (i.e. the one our drive had before we started)
	for (pass = 0; (pass < 2) && (letter == 0); pass++) {
		for (i = 'C' - 'A'; i <= 'Z' - 'A'; i++) {
			if (drives & (1 << i))
				continue;
			if (letter_owner[i] == ((pass == 0) ? (worker_slot + 1) : 0)) {
				letter_owner[i] = (uint8_t)(worker_slot + 1);
				letter = (char)('A' + i);
				break;
			}
		}
	}
	ReleaseMutex(hLetterMutex);
	return letter;
}

/*
 * Snapshot of the main dialog controls. Rather than modifying every place where the format code
 * queries the UI, workers get an invisible replica of the main dialog, with the exact same control
 * states as the master's.
 */
static BOOL CALLBACK SnapshotControl(HWND hCtrl, LPARAM lParam)
{
	mufus_job_t* job = (mufus_job_t*)lParam;
	mufus_ctrl_t* ctrl;
	char class_name[32];
	int i, id = GetDlgCtrlID(hCtrl);
	LONG style;

	// Only consider direct children of the main dialog with a valid ID
	if ((GetParent(hCtrl) != hMainDialog) || (id <= 0) || (id == IDC_DEVICE) || (id == IDC_MULTI_DEVICE) ||
		(GetClassNameA(hCtrl, class_name, sizeof(class_name)) == 0))
		return TRUE;
	if (job->nb_ctrls >= MUFUS_MAX_CTRLS) {
		uprintf("Mufus: Too many controls to snapshot!");
		return FALSE;
	}
	ctrl = &job->ctrl[job->nb_ctrls];
	memset(ctrl, 0, sizeof(*ctrl));
	ctrl->id = id;
	ctrl->enabled = IsWindowEnabled(hCtrl);
	if (_stricmp(class_name, WC_BUTTONA) == 0) {
		style = GetWindowLong(hCtrl, GWL_STYLE) & BS_TYPEMASK;
		if ((style != BS_CHECKBOX) && (style != BS_AUTOCHECKBOX) && (style != BS_3STATE) &&
			(style != BS_AUTO3STATE) && (style != BS_RADIOBUTTON) && (style != BS_AUTORADIOBUTTON))
			return TRUE;
		ctrl->kind = MUFUS_CTRL_BUTTON;
		ctrl->state = Button_GetCheck(hCtrl);
	} else if (_stricmp(class_name, WC_EDITA) == 0) {
		ctrl->kind = MUFUS_CTRL_EDIT;
		GetWindowTextU(hCtrl, ctrl->text, sizeof(ctrl->text));
	} else if (_stricmp(class_name, WC_COMBOBOXA) == 0) {
		ctrl->kind = MUFUS_CTRL_COMBO;
		ctrl->cur_sel = ComboBox_GetCurSel(hCtrl);
		ctrl->nb_items = min(ComboBox_GetCount(hCtrl), MUFUS_MAX_COMBO_ITEMS);
		for (i = 0; i < ctrl->nb_items; i++) {
			ctrl->item[i].data = (LONG_PTR)ComboBox_GetItemData(hCtrl, i);
			// NB: ComboBox_GetLBTextU() writes at most (length + 1) bytes
			if (ComboBox_GetLBTextLen(hCtrl, i) < (int)sizeof(ctrl->item[i].text) - 1)
				ComboBox_GetLBTextU(hCtrl, i, ctrl->item[i].text);
		}
		if (ctrl->cur_sel >= MUFUS_MAX_COMBO_ITEMS) {
			// Should never happen, but make sure we preserve the selected value regardless
			ctrl->item[MUFUS_MAX_COMBO_ITEMS - 1].data = (LONG_PTR)ComboBox_GetItemData(hCtrl, ctrl->cur_sel);
			ctrl->cur_sel = MUFUS_MAX_COMBO_ITEMS - 1;
		}
	} else {
		return TRUE;
	}
	job->nb_ctrls++;
	return TRUE;
}

static void RestoreControls(HWND hDlg, const mufus_job_t* job)
{
	int i, j;
	HWND hCtrl;
	const mufus_ctrl_t* ctrl;

	for (i = 0; i < job->nb_ctrls; i++) {
		ctrl = &job->ctrl[i];
		hCtrl = GetDlgItem(hDlg, ctrl->id);
		if (hCtrl == NULL)
			continue;
		switch (ctrl->kind) {
		case MUFUS_CTRL_BUTTON:
			Button_SetCheck(hCtrl, ctrl->state);
			break;
		case MUFUS_CTRL_EDIT:
			SetWindowTextU(hCtrl, ctrl->text);
			break;
		case MUFUS_CTRL_COMBO:
			IGNORE_RETVAL(ComboBox_ResetContent(hCtrl));
			for (j = 0; j < ctrl->nb_items; j++)
				IGNORE_RETVAL(ComboBox_SetItemData(hCtrl, ComboBox_AddStringU(hCtrl, ctrl->item[j].text), ctrl->item[j].data));
			IGNORE_RETVAL(ComboBox_SetCurSel(hCtrl, ctrl->cur_sel));
			break;
		}
		EnableWindow(hCtrl, ctrl->enabled);
	}
}

static int FindDriveIndex(DWORD device_num)
{
	int i;

	for (i = 0; (i < ComboBox_GetCount(hDeviceList)) && (i < MAX_DRIVES); i++) {
		if ((DWORD)ComboBox_GetItemData(hDeviceList, i) == device_num)
			return i;
	}
	return -1;
}

static BOOL IsDriveLabelEmpty(const char* label)
{
	return (label == NULL) || (label[0] == 0) || (_stricmp(label, STR_NO_LABEL) == 0) ||
		(safe_stricmp(label, lmprintf(MSG_207)) == 0);
}

// The answer to give to a prompt, when the operation is being cancelled
static int CancelAnswer(int type)
{
	switch (type & 0x0F) {
	case MB_ABORTRETRYIGNORE:
		return IDABORT;
	case MB_YESNO:
		return IDNO;
	case MB_OK:
		return IDOK;
	default:
		return IDCANCEL;
	}
}

// Some of the rufus_drive[] strings, such as the hub, may be NULL
#define STR_OR_EMPTY(s) (((s) != NULL) ? (s) : "")

static BOOL IsCancelled(DWORD error_status)
{
	return IS_ERROR(error_status) && (SCODE_CODE(error_status) == ERROR_CANCELLED);
}

/*
 * Build the part of the job that is common to all drives. Must be called from the UI thread.
 */
static mufus_job_t* BuildJob(size_t* job_size)
{
	mufus_job_t* job;
	int primary;
	ULONG cluster_size;
	char label[256];

	*job_size = sizeof(mufus_job_t) + ((grub2_buf != NULL) ? (size_t)grub2_len : 0);
	job = (mufus_job_t*)calloc(1, *job_size);
	if (job == NULL)
		return NULL;
	job->magic = MUFUS_JOB_MAGIC;
	job->version = MUFUS_JOB_VERSION;
	job->struct_size = sizeof(mufus_job_t);
	job->master_pid = GetCurrentProcessId();

	if (image_path != NULL)
		static_strcpy(job->image_path, image_path);
	if (archive_path != NULL)
		static_strcpy(job->archive_path, archive_path);
	if (vhd_mounted) {
		static_strcpy(job->vhd_physical_path, vhd_physical_path);
		job->vhd_size = vhd_size;
	}
	GetWindowTextU(hMainDialog, job->main_title, sizeof(job->main_title));

	job->boot_type = boot_type;
	job->partition_type = partition_type;
	job->target_type = target_type;
	job->fs_type = fs_type;
	job->selection_default = selection_default;
	job->default_thread_priority = default_thread_priority;
	job->update_progress_type = update_progress_type;
	job->unattend_xml_flags = unattend_xml_flags;
	job->unattend_xml_mask = unattend_xml_mask;
	job->unattend_edition_index = unattend_edition_index;
	job->wintogo_index = wintogo_index;
	job->wininst_index = wininst_index;
	memcpy(job->removable_section, removable_section, sizeof(job->removable_section));
	job->image_options = image_options;
	job->write_as_image = write_as_image;
	job->write_as_esp = write_as_esp;
	job->zero_drive = zero_drive;
	job->fast_zeroing = fast_zeroing;
	job->append_silent = append_silent;
	job->lock_drive = lock_drive;
	job->enable_ntfs_compression = enable_ntfs_compression;
	job->enable_iso = enable_iso;
	job->enable_joliet = enable_joliet;
	job->enable_rockridge = enable_rockridge;
	job->validate_md5sum = validate_md5sum;
	job->use_vds = use_vds;
	job->enable_file_indexing = enable_file_indexing;
	job->force_large_fat32 = force_large_fat32;
	job->allow_dual_uefi_bios = allow_dual_uefi_bios;
	job->use_rufus_mbr = use_rufus_mbr;
	job->detect_fakes = detect_fakes;
	job->preserve_timestamps = preserve_timestamps;
	job->usb_debug = usb_debug;
	job->expert_mode = expert_mode;
	job->enable_extra_hashes = enable_extra_hashes;
	job->advanced_mode_device = advanced_mode_device;
	job->advanced_mode_format = advanced_mode_format;
	job->ignore_boot_marker = ignore_boot_marker;
	job->enable_HDDs = enable_HDDs;
	job->enable_VHDs = enable_VHDs;
	job->use_fake_units = use_fake_units;
	job->mbr_selected_by_user = mbr_selected_by_user;
	job->has_uefi_csm = has_uefi_csm;
	job->list_non_usb_removable_drives = list_non_usb_removable_drives;
	job->is_vds_available = is_vds_available;
	memcpy(job->use_own_c32, use_own_c32, sizeof(job->use_own_c32));
	memcpy(job->syslinux_ldlinux_len, syslinux_ldlinux_len, sizeof(job->syslinux_ldlinux_len));
	job->persistence_size = persistence_size;
	job->total_blocks = total_blocks;
	job->extra_blocks = extra_blocks;
	job->dur_mins = dur_mins;
	job->dur_secs = dur_secs;
	memcpy(job->rufus_version, rufus_version, sizeof(job->rufus_version));
	memcpy(job->embedded_sl_version, embedded_sl_version, sizeof(job->embedded_sl_version));
	memcpy(job->embedded_sl_version_str, embedded_sl_version_str, sizeof(job->embedded_sl_version_str));
	memcpy(job->embedded_sl_version_ext, embedded_sl_version_ext, sizeof(job->embedded_sl_version_ext));
	memcpy(&job->img_report, &img_report, sizeof(img_report));

	EnumChildWindows(hMainDialog, SnapshotControl, (LPARAM)job);

	// If the cluster size is the default one for the (primary) drive, let each worker use the
	// default for its own drive. Same for the label, when it was proposed from the drive.
	// After an image scan, Rufus may also leave the cluster sizes of the previous file system
	// listed (it switches the file system while the controls are disabled, so the list doesn't
	// get updated), in which case the selection is the default of that other file system and
	// isn't valid for this one => use the default of each drive then too.
	cluster_size = (ULONG)ComboBox_GetCurItemData(hClusterSize);
	job->auto_cluster_size = ((fs_type >= 0) && (fs_type < FS_MAX) &&
		((cluster_size == SelectedDrive.ClusterSize[fs_type].Default) ||
		!(SelectedDrive.ClusterSize[fs_type].Allowed & cluster_size)));
	primary = ComboBox_GetCurSel(hDeviceList);
	GetWindowTextU(hLabel, label, sizeof(label));
	if ((primary >= 0) && (primary < MAX_DRIVES) &&
		!((boot_type == BT_IMAGE) && (image_path != NULL) && (img_report.label[0] != 0)))
		job->auto_label = (strcmp(label, IsDriveLabelEmpty(rufus_drive[primary].label) ?
			SelectedDrive.proposed_label : rufus_drive[primary].label) == 0);

	if (grub2_buf != NULL) {
		job->grub2_len = (uint32_t)grub2_len;
		memcpy(&((uint8_t*)job)[sizeof(mufus_job_t)], grub2_buf, grub2_len);
	}
	return job;
}

/*
 * Worker side
 */
BOOL MufusParseWorkerArgs(void)
{
	int argc = 0;
	wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);

	if (wargv == NULL)
		return FALSE;
	if ((argc == 4) && (wcscmp(wargv[1], WIDEN(MUFUS_WORKER_ARG)) == 0)) {
		hFromMaster = (HANDLE)(uintptr_t)_wcstoui64(wargv[2], NULL, 16);
		hToMaster = (HANDLE)(uintptr_t)_wcstoui64(wargv[3], NULL, 16);
	}
	LocalFree(wargv);
	if ((hFromMaster == NULL) || (hToMaster == NULL))
		return FALSE;
	// Don't let the processes we start (dism, bcdboot, etc.) inherit our pipes, as this would
	// keep them open after we exit, and prevent the master from detecting that we are done.
	SetHandleInformation(hFromMaster, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(hToMaster, HANDLE_FLAG_INHERIT, 0);
	InitializeCriticalSection(&worker_send_lock);
	InitializeCriticalSection(&worker_prompt_lock);
	mufus_worker = TRUE;
	return TRUE;
}

void MufusWorkerLog(const char* str)
{
	static int timestamps = -1;
	char buf[4096];
	SYSTEMTIME t;

	if (!worker_relay_log || (str == NULL))
		return;
	// Timestamps can be added to the log of the workers, to diagnose timing issues
	if (timestamps < 0)
		timestamps = (GetEnvironmentVariableA("MUFUS_LOG_TIMESTAMPS", NULL, 0) != 0);
	if (timestamps && (strlen(str) < sizeof(buf) - 16)) {
		GetLocalTime(&t);
		static_sprintf(buf, "%02u:%02u:%02u.%03u %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, str);
		str = buf;
	}
	SendMsg(&hToMaster,&worker_send_lock, MUFUS_MSG_LOG, str, (uint32_t)strlen(str));
}

void MufusWorkerLogW(const wchar_t* wstr)
{
	char* str;

	if (!worker_relay_log || (wstr == NULL))
		return;
	str = wchar_to_utf8(wstr);
	MufusWorkerLog(str);
	free(str);
}

void MufusWorkerProgress(int pos)
{
	int32_t val = pos;

	if (worker_relay_log)
		SendMsg(&hToMaster,&worker_send_lock, MUFUS_MSG_PROGRESS, &val, sizeof(val));
}

void MufusWorkerInfo(BOOL info, const char* msg)
{
	if (worker_relay_log && (msg != NULL))
		SendMsg(&hToMaster,&worker_send_lock, info ? MUFUS_MSG_INFO : MUFUS_MSG_STATUS, msg, (uint32_t)strlen(msg));
}

int MufusWorkerPrompt(int type, const char* title, const char* message)
{
	int r;
	size_t title_len = safe_strlen(title), message_len = safe_strlen(message);
	uint32_t size = (uint32_t)(sizeof(int32_t) + title_len + message_len + 2);
	uint8_t* buf = (uint8_t*)malloc(size);

	if (buf == NULL)
		return IDCANCEL;
	*(int32_t*)buf = type;
	memcpy(&buf[sizeof(int32_t)], (title != NULL) ? title : "", title_len + 1);
	memcpy(&buf[sizeof(int32_t) + title_len + 1], (message != NULL) ? message : "", message_len + 1);
	// Only one prompt at a time
	EnterCriticalSection(&worker_prompt_lock);
	ResetEvent(hPromptEvent);
	prompt_result = CancelAnswer(type);
	if (worker_cancel_requested) {
		// No point in asking anything if we are being cancelled
		r = prompt_result;
		LeaveCriticalSection(&worker_prompt_lock);
		free(buf);
		return r;
	}
	worker_prompt_type = type;
	if (SendMsg(&hToMaster,&worker_send_lock, MUFUS_MSG_PROMPT, buf, size))
		WaitForSingleObject(hPromptEvent, INFINITE);
	worker_prompt_type = -1;
	r = prompt_result;
	LeaveCriticalSection(&worker_prompt_lock);
	free(buf);
	return r;
}

const char* MufusHiveName(const char* default_name)
{
	return (mufus_worker && (worker_hive_name[0] != 0)) ? worker_hive_name : default_name;
}

// Workers use the VHD that the master mounted, since a VHD can only be attached once
char* MufusGetVhdSource(uint64_t* disk_size)
{
	if (!mufus_worker || (worker_vhd_path[0] == 0))
		return NULL;
	if (disk_size != NULL)
		*disk_size = worker_vhd_size;
	return worker_vhd_path;
}

// Listens for commands from the master
static DWORD WINAPI WorkerListenerThread(LPVOID param)
{
	uint8_t* data;
	uint32_t type, size;

	while ((data = RecvMsg(hFromMaster, &type, &size)) != NULL) {
		switch (type) {
		case MUFUS_MSG_CANCEL:
			worker_cancel_requested = TRUE;
			ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
			uprintf("Cancelling");
			// Answer any pending prompt, so that we don't wait for the user
			if (worker_prompt_type >= 0) {
				prompt_result = CancelAnswer(worker_prompt_type);
				SetEvent(hPromptEvent);
			}
			break;
		case MUFUS_MSG_PROMPT_REPLY:
			if (size >= sizeof(int32_t)) {
				prompt_result = *(int32_t*)data;
				SetEvent(hPromptEvent);
			}
			break;
		default:
			break;
		}
		free(data);
	}
	// If we lost the connection to the master, we have no means of reporting
	// anything anymore, so the best course of action is to cancel.
	worker_cancel_requested = TRUE;
	ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
	if (worker_prompt_type >= 0)
		prompt_result = CancelAnswer(worker_prompt_type);
	SetEvent(hPromptEvent);
	return 0;
}

static INT_PTR CALLBACK WorkerCallback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_INITDIALOG:
		return (INT_PTR)TRUE;
	case UM_FORMAT_COMPLETED:
		format_thread = NULL;
		PostQuitMessage(0);
		return (INT_PTR)TRUE;
	case WM_CLOSE:
	case WM_QUERYENDSESSION:
	case WM_ENDSESSION:
		// Never let a worker be closed while it is writing a drive
		return (INT_PTR)TRUE;
	}
	return (INT_PTR)FALSE;
}

// Return the current disk number of the device with instance ID 'id', -1 if this device
// isn't present, or -2 if the device is present but its disk number couldn't be obtained.
static int GetDiskNumberFromId(const char* id)
{
	HDEVINFO dev_info;
	SP_DEVINFO_DATA dev_info_data;
	SP_DEVICE_INTERFACE_DATA devint_data;
	PSP_DEVICE_INTERFACE_DETAIL_DATA_A devint_detail_data = NULL;
	char instance_id[MAX_PATH];
	DWORD i, size;
	HANDLE hDrive;
	int number = -2;
	BOOL found = FALSE;

	dev_info = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_DISK, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (dev_info == INVALID_HANDLE_VALUE)
		return -2;
	dev_info_data.cbSize = sizeof(dev_info_data);
	for (i = 0; !found && SetupDiEnumDeviceInfo(dev_info, i, &dev_info_data); i++) {
		if (!SetupDiGetDeviceInstanceIdA(dev_info, &dev_info_data, instance_id, sizeof(instance_id), &size) ||
			(_stricmp(instance_id, id) != 0))
			continue;
		found = TRUE;
		devint_data.cbSize = sizeof(devint_data);
		if (!SetupDiEnumDeviceInterfaces(dev_info, &dev_info_data, &GUID_DEVINTERFACE_DISK, 0, &devint_data))
			break;
		size = 0;
		SetupDiGetDeviceInterfaceDetailA(dev_info, &devint_data, NULL, 0, &size, NULL);
		devint_detail_data = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)calloc(1, max(size, sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A)));
		if (devint_detail_data == NULL)
			break;
		devint_detail_data->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
		if (!SetupDiGetDeviceInterfaceDetailA(dev_info, &devint_data, devint_detail_data, size, &size, NULL))
			break;
		hDrive = CreateFileA(devint_detail_data->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hDrive == INVALID_HANDLE_VALUE)
			break;
		number = GetDriveNumber(hDrive, devint_detail_data->DevicePath);
		if (number < 0)
			number = -2;
		CloseHandle(hDrive);
	}
	free(devint_detail_data);
	SetupDiDestroyDeviceInfoList(dev_info);
	return found ? number : -1;
}

static BOOL HasDeviceId(const char* id)
{
	return (id != NULL) && (id[0] != 0) && (strcmp(id, "<N/A>") != 0);
}

// Make sure that the disk we are about to write is still the one that was selected, since disk
// numbers are reassigned if devices are re-enumerated (e.g. if a stick is reseated or a hub resets).
static BOOL VerifyDriveIdentity(DWORD device_num, const char* id)
{
	int number;

	if (!HasDeviceId(id)) {
		uprintf("WARNING: Unable to verify the identity of the drive (no device ID)");
		return TRUE;
	}
	number = GetDiskNumberFromId(id);
	if (number == -1) {
		uprintf("The selected drive is no longer present");
		return FALSE;
	}
	if ((number < 0) || ((DWORD)number + DRIVE_INDEX_MIN != device_num)) {
		uprintf("The selected drive is no longer disk %lu (it is now %d)", device_num - DRIVE_INDEX_MIN, number);
		return FALSE;
	}
	return TRUE;
}

/*
 * Cycling a device (disabling and re-enabling it) gives it a new disk number when it comes back.
 * So, if several workers were to cycle their devices at the same time, their disks could swap
 * numbers, and a worker would end up writing to another worker's drive. To prevent that, device
 * cycling is serialized across workers, and the identity of the drive is checked afterwards.
 */
void MufusBeginCycle(void)
{
	if (hCycleMutex != NULL)
		WaitForSingleObject(hCycleMutex, 120000);
}

BOOL MufusEndCycle(DWORD DriveIndex)
{
	int i, number = -1;
	BOOL r = TRUE;

	if (HasDeviceId(rufus_drive[0].id)) {
		// Wait for the device to come back
		for (i = 0; i < 60; i++) {
			number = GetDiskNumberFromId(rufus_drive[0].id);
			if (number >= 0)
				break;
			Sleep(250);
		}
		r = (number >= 0) && ((DWORD)number + DRIVE_INDEX_MIN == DriveIndex);
		if (!r)
			uprintf("The drive did not come back as disk %lu after cycling (%d) - aborting", DriveIndex - DRIVE_INDEX_MIN, number);
	}
	if (hCycleMutex != NULL)
		ReleaseMutex(hCycleMutex);
	return r;
}

/*
 * Returns TRUE if the drive has no partition at all (blank or wiped drive). Asking VDS to delete
 * the partitions of several such drives at once can stall for minutes (VDS doesn't know about
 * blank drives, and every request but one then waits for a 2 minutes timeout), and since there
 * is nothing to delete, workers skip that step for these drives.
 */
BOOL MufusDriveHasNoPartitions(DWORD DriveIndex)
{
	BYTE layout[4096] = { 0 };
	PDRIVE_LAYOUT_INFORMATION_EX DriveLayout = (PDRIVE_LAYOUT_INFORMATION_EX)(void*)layout;
	HANDLE hPhysical;
	DWORD i, size;
	BOOL r;

	hPhysical = GetPhysicalHandle(DriveIndex, FALSE, FALSE, TRUE);
	if ((hPhysical == INVALID_HANDLE_VALUE) || (hPhysical == NULL))
		return FALSE;
	r = DeviceIoControl(hPhysical, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, layout, sizeof(layout), &size, NULL);
	CloseHandle(hPhysical);
	// If the layout can't be read (including if there are too many partitions for our buffer),
	// report that there are partitions, so that the regular deletion is attempted.
	if (!r || (DriveLayout->PartitionStyle == PARTITION_STYLE_GPT && DriveLayout->PartitionCount != 0))
		return FALSE;
	if (DriveLayout->PartitionStyle == PARTITION_STYLE_MBR) {
		for (i = 0; i < DriveLayout->PartitionCount; i++) {
			if (DriveLayout->PartitionEntry[i].Mbr.PartitionType != PARTITION_ENTRY_UNUSED)
				return FALSE;
		}
	}
	return TRUE;
}

/*
 * Called by workers when FormatEx() reports that the new volume is in use, before retrying.
 * Windows assigns a drive letter to a new volume as soon as it appears, after which Explorer
 * and other services may access it (e.g. to prompt for it to be formatted), and with several
 * drives being written at once, they get more of a chance to do so before we format it.
 * So report what is accessing the volume, then remove its drive letter(s) and dismount it,
 * which invalidates the handles that other processes hold on it (it holds no data yet).
 */
void MufusFormatInUse(DWORD DriveIndex, uint64_t PartitionOffset)
{
	char letters[27] = { 0 };
	HANDLE hVolume;

	if (GetDriveLetters(DriveIndex, letters) && (letters[0] != 0))
		uprintf("Drive letter(s) assigned by Windows: %s", letters);
	if (GetProcessSearch(SEARCH_PROCESS_TIMEOUT, 0x07, FALSE) == 0)
		uprintf("No conflicting process found");
	RemoveDriveLetters(DriveIndex, FALSE, TRUE);
	hVolume = GetLogicalHandle(DriveIndex, PartitionOffset, FALSE, TRUE, TRUE);
	if ((hVolume != NULL) && (hVolume != INVALID_HANDLE_VALUE)) {
		UnmountVolume(hVolume);
		CloseHandle(hVolume);
	}
}

static DWORD WINAPI RefreshLayoutThread(LPVOID param)
{
	// Unlike RefreshLayout(), VdsRescan() doesn't alter ErrorStatus, which matters since we may
	// not wait for it to complete. Note that this skips the system-wide cleanup of obsolete mount
	// points that RefreshLayout() also performs, which workers shouldn't all request concurrently.
	VdsRescan(VDS_RESCAN_REFRESH | VDS_RESCAN_REENUMERATE, 0, TRUE);
	return 0;
}

/*
 * After a blank (uninitialized) disk gets partitioned, VDS may take minutes to service any new
 * request, which, with multiple drives being written, would stall every other worker that asks
 * VDS to refresh the layout. As the new partition gets reported through the regular PnP and
 * mount manager notifications anyway (which is what Rufus relies on when VDS isn't available),
 * we don't wait for the refresh to complete for more than a few seconds.
 */
void MufusRefreshLayout(void)
{
	HANDLE hThread = CreateThread(NULL, 0, RefreshLayoutThread, NULL, 0, NULL);

	if (hThread == NULL) {
		VdsRescan(VDS_RESCAN_REFRESH | VDS_RESCAN_REENUMERATE, 0, TRUE);
		return;
	}
	if (WaitForSingleObject(hThread, 10000) == WAIT_TIMEOUT)
		uprintf("VDS is busy - continuing without waiting for the layout refresh");
	CloseHandle(hThread);
}

// Rufus may reset ErrorStatus in places (e.g. after failing to delete partitions), so we
// keep re-asserting a cancellation request, for as long as the format thread is running.
static void CALLBACK CancelTimer(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	if (worker_cancel_requested && (format_thread != NULL) && !IsCancelled(ErrorStatus))
		ErrorStatus = RUFUS_ERROR(ERROR_CANCELLED);
}

int MufusWorkerMain(void)
{
	uint8_t* data = NULL;
	uint32_t type = 0, size = 0;
	HWND hDlg = NULL;
	HANDLE hListener = NULL;
	MSG msg;
	char fs_name[32];
	uint32_t result[2] = { RUFUS_ERROR(ERROR_GEN_FAILURE), 0 };
	uint64_t start_time;
	mufus_job_t* job;

	hPromptEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	data = RecvMsg(hFromMaster, &type, &size);
	job = (mufus_job_t*)data;
	if ((data == NULL) || (type != MUFUS_MSG_JOB) || (size < sizeof(mufus_job_t)) || (job->magic != MUFUS_JOB_MAGIC) ||
		(job->version != MUFUS_JOB_VERSION) || (job->struct_size != sizeof(mufus_job_t)) ||
		(size != sizeof(mufus_job_t) + job->grub2_len)) {
		uprintf("Mufus worker: Invalid job received");
		goto out;
	}

	// From now on, our log is relayed to the master
	worker_relay_log = TRUE;
	worker_slot = job->slot;
	{
		FILETIME creation, exit_time, kernel, user, now;
		uint64_t ms = 0;
		if (GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user)) {
			GetSystemTimeAsFileTime(&now);
			ms = ((((uint64_t)now.dwHighDateTime) << 32 | now.dwLowDateTime) -
				(((uint64_t)creation.dwHighDateTime) << 32 | creation.dwLowDateTime)) / 10000;
		}
		uprintf("Worker started for %s (in %llu ms)", job->drive_display_name, ms);
	}
	if (!OpenLetterTable(job->master_pid, FALSE))
		uprintf("WARNING: Could not access the drive letters table: %s", WindowsErrorString());
	{
		char name[64];
		static_sprintf(name, "Local\\Mufus_Cycle_%08lX", job->master_pid);
		hCycleMutex = CreateMutexA(NULL, FALSE, name);
	}

	// Restore the master's state
	safe_free(image_path);
	if (job->image_path[0] != 0)
		image_path = safe_strdup(job->image_path);
	safe_free(archive_path);
	if (job->archive_path[0] != 0)
		archive_path = safe_strdup(job->archive_path);
	unattend_xml_path = (job->unattend_xml_path[0] != 0) ? safe_strdup(job->unattend_xml_path) : NULL;
	static_strcpy(worker_hive_name, job->hive_name);
	static_strcpy(worker_vhd_path, job->vhd_physical_path);
	worker_vhd_size = job->vhd_size;
	boot_type = job->boot_type;
	partition_type = job->partition_type;
	target_type = job->target_type;
	fs_type = job->fs_type;
	selection_default = job->selection_default;
	default_thread_priority = job->default_thread_priority;
	update_progress_type = job->update_progress_type;
	unattend_xml_flags = job->unattend_xml_flags;
	unattend_xml_mask = job->unattend_xml_mask;
	unattend_edition_index = job->unattend_edition_index;
	wintogo_index = job->wintogo_index;
	wininst_index = job->wininst_index;
	memcpy(removable_section, job->removable_section, sizeof(removable_section));
	image_options = job->image_options;
	write_as_image = job->write_as_image;
	write_as_esp = job->write_as_esp;
	zero_drive = job->zero_drive;
	fast_zeroing = job->fast_zeroing;
	append_silent = job->append_silent;
	lock_drive = job->lock_drive;
	enable_ntfs_compression = job->enable_ntfs_compression;
	enable_iso = job->enable_iso;
	enable_joliet = job->enable_joliet;
	enable_rockridge = job->enable_rockridge;
	validate_md5sum = job->validate_md5sum;
	is_vds_available = job->is_vds_available;
	use_vds = job->use_vds && is_vds_available;
	enable_file_indexing = job->enable_file_indexing;
	force_large_fat32 = job->force_large_fat32;
	allow_dual_uefi_bios = job->allow_dual_uefi_bios;
	use_rufus_mbr = job->use_rufus_mbr;
	detect_fakes = job->detect_fakes;
	preserve_timestamps = job->preserve_timestamps;
	usb_debug = job->usb_debug;
	expert_mode = job->expert_mode;
	enable_extra_hashes = job->enable_extra_hashes;
	advanced_mode_device = job->advanced_mode_device;
	advanced_mode_format = job->advanced_mode_format;
	ignore_boot_marker = job->ignore_boot_marker;
	enable_HDDs = job->enable_HDDs;
	enable_VHDs = job->enable_VHDs;
	use_fake_units = job->use_fake_units;
	mbr_selected_by_user = job->mbr_selected_by_user;
	has_uefi_csm = job->has_uefi_csm;
	list_non_usb_removable_drives = job->list_non_usb_removable_drives;
	memcpy(use_own_c32, job->use_own_c32, sizeof(use_own_c32));
	memcpy(syslinux_ldlinux_len, job->syslinux_ldlinux_len, sizeof(syslinux_ldlinux_len));
	persistence_size = job->persistence_size;
	total_blocks = job->total_blocks;
	extra_blocks = job->extra_blocks;
	dur_mins = job->dur_mins;
	dur_secs = job->dur_secs;
	memcpy(rufus_version, job->rufus_version, sizeof(rufus_version));
	memcpy(embedded_sl_version, job->embedded_sl_version, sizeof(embedded_sl_version));
	memcpy(embedded_sl_version_str, job->embedded_sl_version_str, sizeof(embedded_sl_version_str));
	memcpy(embedded_sl_version_ext, job->embedded_sl_version_ext, sizeof(embedded_sl_version_ext));
	memcpy(&img_report, &job->img_report, sizeof(img_report));
	cdio_loglevel_default = usb_debug ? CDIO_LOG_INFO : CDIO_LOG_WARN;
	// Same as what InitDialog() does for the master
	StrArrayCreate(&BlockingProcessList, 16);
	StrArrayCreate(&ImageList, 16);
	StrArrayCreate(&modified_files, 8);
	safe_free(grub2_buf);
	grub2_len = 0;
	if (job->grub2_len != 0) {
		grub2_buf = (uint8_t*)malloc(job->grub2_len);
		if (grub2_buf != NULL) {
			memcpy(grub2_buf, &data[sizeof(mufus_job_t)], job->grub2_len);
			grub2_len = (long)job->grub2_len;
		}
	}

	// Create an invisible replica of the main dialog
	hDlg = MyCreateDialog(hMainInstance, IDD_DIALOG, NULL, WorkerCallback);
	if (hDlg == NULL) {
		uprintf("Mufus worker: Could not create dialog: %s", WindowsErrorString());
		goto out;
	}
	hMainDialog = hDlg;
	MainThreadId = GetCurrentThreadId();
	hDeviceList = GetDlgItem(hDlg, IDC_DEVICE);
	hPartitionScheme = GetDlgItem(hDlg, IDC_PARTITION_TYPE);
	hTargetSystem = GetDlgItem(hDlg, IDC_TARGET_SYSTEM);
	hFileSystem = GetDlgItem(hDlg, IDC_FILE_SYSTEM);
	hClusterSize = GetDlgItem(hDlg, IDC_CLUSTER_SIZE);
	hLabel = GetDlgItem(hDlg, IDC_LABEL);
	hProgress = GetDlgItem(hDlg, IDC_PROGRESS);
	hBootType = GetDlgItem(hDlg, IDC_BOOT_SELECTION);
	hImageOption = GetDlgItem(hDlg, IDC_IMAGE_OPTION);
	hNBPasses = GetDlgItem(hDlg, IDC_NB_PASSES);
	SetWindowTextU(hDlg, job->main_title);
	SendMessage(hProgress, PBM_SETRANGE, 0, (MAX_PROGRESS << 16) & 0xFFFF0000);
	RestoreControls(hDlg, job);
	imop_win_sel = ComboBox_GetCurSel(hImageOption);

	// Our single device. The format code expects the combo index to match the rufus_drive[] index.
	memset(&rufus_drive[0], 0, sizeof(RUFUS_DRIVE));
	rufus_drive[0].id = (job->drive_id[0] != 0) ? safe_strdup(job->drive_id) : NULL;
	rufus_drive[0].name = (job->drive_name[0] != 0) ? safe_strdup(job->drive_name) : NULL;
	rufus_drive[0].display_name = (job->drive_display_name[0] != 0) ? safe_strdup(job->drive_display_name) : NULL;
	rufus_drive[0].label = (job->drive_label[0] != 0) ? safe_strdup(job->drive_label) : NULL;
	rufus_drive[0].hub = (job->drive_hub[0] != 0) ? safe_strdup(job->drive_hub) : NULL;
	rufus_drive[0].index = job->drive_index;
	rufus_drive[0].port = job->drive_port;
	rufus_drive[0].size = job->drive_size;
	IGNORE_RETVAL(ComboBox_ResetContent(hDeviceList));
	IGNORE_RETVAL(ComboBox_SetItemData(hDeviceList, ComboBox_AddStringU(hDeviceList, job->device_text), job->device_num));
	IGNORE_RETVAL(ComboBox_SetCurSel(hDeviceList, 0));

	// Before we do anything with our drive, make sure that it is the one that was selected
	if (!VerifyDriveIdentity(job->device_num, job->drive_id)) {
		result[0] = RUFUS_ERROR(ERROR_DEV_NOT_EXIST);
		goto out;
	}
	if (!zero_drive && (boot_type == BT_IMAGE) && (image_path != NULL) && IsSourceImageLocatedOnTargetDrive(job->device_num)) {
		uprintf("The source image is located on this drive!");
		result[0] = RUFUS_ERROR(ERROR_ACCESS_DENIED);
		goto out;
	}

	// Get the properties of our drive, like PopulateProperties() does in the master
	memset(&SelectedDrive, 0, sizeof(SelectedDrive));
	SelectedDrive.DeviceNumber = job->device_num;
	GetDrivePartitionData(SelectedDrive.DeviceNumber, fs_name, sizeof(fs_name), FALSE);
	if (SelectedDrive.DiskSize == 0) {
		uprintf("Could not access drive 0x%02lX", job->device_num);
		result[0] = RUFUS_ERROR(ERROR_OPEN_FAILED);
		goto out;
	}
	static_sprintf(SelectedDrive.proposed_label, "%s", SizeToHumanReadable(SelectedDrive.DiskSize, FALSE, TRUE));
	ComputeClusterSizes();
	if (job->auto_cluster_size && (fs_type >= 0) && (fs_type < FS_MAX) && (SelectedDrive.ClusterSize[fs_type].Default != 0)) {
		// Use the default cluster size for this specific drive
		IGNORE_RETVAL(ComboBox_ResetContent(hClusterSize));
		IGNORE_RETVAL(ComboBox_SetItemData(hClusterSize, ComboBox_AddStringU(hClusterSize, "Default"),
			SelectedDrive.ClusterSize[fs_type].Default));
		IGNORE_RETVAL(ComboBox_SetCurSel(hClusterSize, 0));
	}
	if (job->auto_label)
		SetWindowTextU(hLabel, IsDriveLabelEmpty(rufus_drive[0].label) ? SelectedDrive.proposed_label : rufus_drive[0].label);

	// Start the conflicting process search thread. It stays idle until GetProcessSearch() is
	// called, which only happens when Rufus has trouble getting exclusive access to the drive.
	if (!StartProcessSearch())
		uprintf("Failed to start conflicting process search");

	// Listen for cancellation and prompt replies from the master
	hListener = CreateThread(NULL, 0, WorkerListenerThread, NULL, 0, NULL);
	if (hListener == NULL) {
		uprintf("Unable to start listener thread");
		result[0] = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
		goto out;
	}

	// Now run the regular Rufus format process, exactly as the master would, unless
	// we have already been asked to cancel
	if (worker_cancel_requested) {
		uprintf("Operation cancelled before it started");
		result[0] = RUFUS_ERROR(ERROR_CANCELLED);
		goto out;
	}
	ErrorStatus = 0;
	LastWriteError = 0;
	InitProgress(zero_drive || write_as_image);
	format_thread = CreateThread(NULL, 0, FormatThread, (LPVOID)(uintptr_t)job->device_num, 0, NULL);
	if (format_thread == NULL) {
		uprintf("Unable to start formatting thread");
		result[0] = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
		goto out;
	}
	SetThreadPriority(format_thread, default_thread_priority);
	SetTimer(hDlg, TID_MUFUS_CANCEL, 250, CancelTimer);
	uprintf("\r\nFormat operation started");
	start_time = GetTickCount64();
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	KillTimer(hDlg, TID_MUFUS_CANCEL);
	uprintf("Format operation %s after %llu seconds", IS_ERROR(ErrorStatus) ? "ended" : "completed",
		(GetTickCount64() - start_time) / 1000);
	result[0] = ErrorStatus;
	result[1] = LastWriteError;

out:
	SendMsg(&hToMaster,&worker_send_lock, MUFUS_MSG_DONE, result, sizeof(result));
	worker_relay_log = FALSE;
	safe_closehandle(hCycleMutex);
	if (hDlg != NULL)
		DestroyWindow(hDlg);
	CloseLetterTable();
	safe_free(data);
	// Don't wait for the listener, as it is blocked on a pipe read
	safe_closehandle(hListener);
	return IS_ERROR(result[0]) ? 1 : 0;
}

/*
 * Master side: worker management
 */
static void PostSlotUpdate(int i)
{
	// Must be called with the slot lock held
	if (!slot[i].update_posted && PostMessage(hMainDialog, UM_MUFUS_UPDATE, (WPARAM)i, 0))
		slot[i].update_posted = TRUE;
}

// Append text to the slot's log buffer, prefixing each line with the slot's tag
static void AppendSlotLog(mufus_slot_t* s, const char* text, size_t len)
{
	size_t i, tag_len = strlen(s->tag), needed;
	char* p;

	// Worst case: each character is the start of a new line
	needed = s->log_len + len + (tag_len + 3) * (len + 1) + 1;
	if (needed > MUFUS_MAX_LOG_BUFFER)
		return;
	if (needed > s->log_max) {
		p = (char*)realloc(s->log, needed + 4 * KB);
		if (p == NULL)
			return;
		s->log = p;
		s->log_max = needed + 4 * KB;
	}
	for (i = 0; i < len; i++) {
		if (s->log_at_line_start && (text[i] != '\r') && (text[i] != '\n')) {
			s->log[s->log_len++] = '[';
			memcpy(&s->log[s->log_len], s->tag, tag_len);
			s->log_len += tag_len;
			s->log[s->log_len++] = ']';
			s->log[s->log_len++] = ' ';
			s->log_at_line_start = FALSE;
		}
		s->log[s->log_len++] = text[i];
		if (text[i] == '\n')
			s->log_at_line_start = TRUE;
	}
	s->log[s->log_len] = 0;
}

static DWORD WINAPI SlotReaderThread(LPVOID param)
{
	int i = (int)(uintptr_t)param;
	mufus_slot_t* s = &slot[i];
	mufus_prompt_t* prompt;
	uint8_t* data;
	uint32_t type, size, reply;

	// The job is larger than the pipe buffer, so this blocks until the worker reads it
	if (!SendMsg(&s->hToWorker,&s->send_lock, MUFUS_MSG_JOB, s->job, (uint32_t)s->job_size))
		uprintf("[%s] Could not send job to worker: %s", s->tag, WindowsErrorString());

	while ((data = RecvMsg(s->hFromWorker, &type, &size)) != NULL) {
		switch (type) {
		case MUFUS_MSG_LOG:
			EnterCriticalSection(&s->lock);
			AppendSlotLog(s, (char*)data, size);
			PostSlotUpdate(i);
			LeaveCriticalSection(&s->lock);
			break;
		case MUFUS_MSG_PROGRESS:
			if (size < sizeof(int32_t))
				break;
			EnterCriticalSection(&s->lock);
			s->pos = max(0, min(MAX_PROGRESS, *(int32_t*)data));
			PostSlotUpdate(i);
			LeaveCriticalSection(&s->lock);
			break;
		case MUFUS_MSG_INFO:
			EnterCriticalSection(&s->lock);
			static_strcpy(s->info, (char*)data);
			PostSlotUpdate(i);
			LeaveCriticalSection(&s->lock);
			break;
		case MUFUS_MSG_STATUS:
			// The status bar only holds transient messages, which we don't need to display
			break;
		case MUFUS_MSG_PROMPT:
			prompt = (mufus_prompt_t*)calloc(1, sizeof(mufus_prompt_t));
			// The payload is the prompt type, followed by the NUL terminated title and message
			if ((prompt != NULL) && (size >= sizeof(int32_t) + 2) &&
				(sizeof(int32_t) + strnlen((char*)&data[sizeof(int32_t)], size - sizeof(int32_t)) < size)) {
				prompt->type = *(int32_t*)data;
				prompt->title = safe_strdup((char*)&data[sizeof(int32_t)]);
				prompt->message = safe_strdup((char*)&data[sizeof(int32_t) + strlen((char*)&data[sizeof(int32_t)]) + 1]);
				if ((prompt->title != NULL) && (prompt->message != NULL) &&
					PostMessage(hMainDialog, UM_MUFUS_PROMPT, (WPARAM)i, (LPARAM)prompt))
					break;
				safe_free(prompt->title);
				safe_free(prompt->message);
			}
			safe_free(prompt);
			reply = IDCANCEL;
			SendMsg(&s->hToWorker,&s->send_lock, MUFUS_MSG_PROMPT_REPLY, &reply, sizeof(reply));
			break;
		case MUFUS_MSG_DONE:
			if (size < 2 * sizeof(uint32_t))
				break;
			EnterCriticalSection(&s->lock);
			s->error_status = ((uint32_t*)data)[0];
			s->done_received = TRUE;
			PostSlotUpdate(i);
			LeaveCriticalSection(&s->lock);
			break;
		default:
			break;
		}
		free(data);
	}
	return 0;
}

static BOOL LaunchWorker(int i)
{
	mufus_slot_t* s = &slot[i];
	SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
	HANDLE hChildRead = NULL, hChildWrite = NULL, handles[2];
	STARTUPINFOEXW si;
	PROCESS_INFORMATION pi;
	SIZE_T attr_size = 0;
	LPPROC_THREAD_ATTRIBUTE_LIST attr = NULL;
	wchar_t exe[MAX_PATH], cmd[MAX_PATH + 128];
	BOOL r = FALSE;

	if (!CreatePipe(&hChildRead, &s->hToWorker, &sa, MUFUS_PIPE_SIZE) ||
		!CreatePipe(&s->hFromWorker, &hChildWrite, &sa, MUFUS_PIPE_SIZE)) {
		uprintf("Could not create pipes: %s", WindowsErrorString());
		goto out;
	}
	// Only the worker's ends of the pipes must be inherited, and nothing else
	SetHandleInformation(s->hToWorker, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(s->hFromWorker, HANDLE_FLAG_INHERIT, 0);
	handles[0] = hChildRead;
	handles[1] = hChildWrite;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
	attr = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
	if ((attr == NULL) || !InitializeProcThreadAttributeList(attr, 1, 0, &attr_size) ||
		!UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), NULL, NULL)) {
		uprintf("Could not set up worker attributes: %s", WindowsErrorString());
		safe_free(attr);
		goto out;
	}
	if (GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe)) == 0)
		goto out;
	_snwprintf_s(cmd, ARRAYSIZE(cmd), _TRUNCATE, L"\"%s\" " WIDEN(MUFUS_WORKER_ARG) L" %llx %llx", exe,
		(unsigned long long)(uintptr_t)hChildRead, (unsigned long long)(uintptr_t)hChildWrite);
	memset(&si, 0, sizeof(si));
	si.StartupInfo.cb = sizeof(si);
	si.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
	si.StartupInfo.wShowWindow = SW_HIDE;
	si.lpAttributeList = attr;
	if (!CreateProcessW(exe, cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) {
		uprintf("Could not launch worker for %s: %s", s->name, WindowsErrorString());
		goto out;
	}
	CloseHandle(pi.hThread);
	s->hProcess = pi.hProcess;
	// Close our copies of the worker ends, so that we get notified when the worker exits
	safe_closehandle(hChildRead);
	safe_closehandle(hChildWrite);
	// The reader thread sends the job, then relays the worker's messages
	s->hReader = CreateThread(NULL, 0, SlotReaderThread, (LPVOID)(uintptr_t)i, 0, NULL);
	if (s->hReader == NULL) {
		uprintf("Could not start reader thread for %s", s->name);
		TerminateProcess(s->hProcess, 1);
		goto out;
	}
	r = TRUE;

out:
	if (attr != NULL) {
		DeleteProcThreadAttributeList(attr);
		free(attr);
	}
	safe_closehandle(hChildRead);
	safe_closehandle(hChildWrite);
	return r;
}

// NB: We don't use safe_closehandle() for these, since we test for NULL handles (and since
// INVALID_HANDLE_VALUE also happens to be the pseudo handle of the current process).
static void CloseSlotHandles(mufus_slot_t* s)
{
	HANDLE* h[] = { &s->hFromWorker, &s->hReader, &s->hProcess };
	int i;

	// The UI thread may be sending a prompt reply to the worker
	EnterCriticalSection(&s->send_lock);
	if ((s->hToWorker != NULL) && (s->hToWorker != INVALID_HANDLE_VALUE))
		CloseHandle(s->hToWorker);
	s->hToWorker = NULL;
	LeaveCriticalSection(&s->send_lock);
	for (i = 0; i < ARRAYSIZE(h); i++) {
		if ((*h[i] != NULL) && (*h[i] != INVALID_HANDLE_VALUE))
			CloseHandle(*h[i]);
		*h[i] = NULL;
	}
}

static DWORD WINAPI SupervisorThread(LPVOID param)
{
	int i, n, nb_failed = 0, nb_cancelled = 0;
	HANDLE h[MUFUS_MAX_DRIVES];
	DWORD error_status = 0, exit_code;
	BOOL cancel_sent = FALSE, crashed;

	for (i = 0; i < nb_slots; i++) {
		if ((i > 0) && !IsCancelled(ErrorStatus))
			Sleep(MUFUS_LAUNCH_STAGGER);
		if (IsCancelled(ErrorStatus))
			break;
		if (LaunchWorker(i)) {
			uprintf("[%s] Worker launched (%.1f s)", slot[i].tag, (GetTickCount64() - multi_start_time) / 1000.0f);
		} else {
			EnterCriticalSection(&slot[i].lock);
			slot[i].error_status = RUFUS_ERROR(APPERR(ERROR_CANT_START_THREAD));
			slot[i].done_received = TRUE;
			PostSlotUpdate(i);
			LeaveCriticalSection(&slot[i].lock);
			if (slot[i].hProcess != NULL) {
				// The worker started, but we couldn't communicate with it. Since it never got
				// its job, it hasn't touched its drive, so it is safe to terminate it.
				if (WaitForSingleObject(slot[i].hProcess, 5000) != WAIT_OBJECT_0)
					TerminateProcess(slot[i].hProcess, 1);
				if (slot[i].hReader != NULL)
					WaitForSingleObject(slot[i].hReader, 5000);
			}
			CloseSlotHandles(&slot[i]);
			ReleaseLetters(i);
		}
	}
	// Drives we never got to start, due to cancellation
	for (; i < nb_slots; i++) {
		EnterCriticalSection(&slot[i].lock);
		slot[i].error_status = RUFUS_ERROR(ERROR_CANCELLED);
		slot[i].done_received = TRUE;
		PostSlotUpdate(i);
		LeaveCriticalSection(&slot[i].lock);
	}

	// Wait for all the workers to complete
	while (TRUE) {
		for (i = 0, n = 0; i < nb_slots; i++) {
			if (slot[i].hProcess != NULL)
				h[n++] = slot[i].hProcess;
		}
		if (n == 0)
			break;
		WaitForMultipleObjects(n, h, FALSE, 250);
		if (!cancel_sent && IsCancelled(ErrorStatus)) {
			for (i = 0; i < nb_slots; i++) {
				if (slot[i].hProcess != NULL)
					SendMsg(&slot[i].hToWorker,&slot[i].send_lock, MUFUS_MSG_CANCEL, NULL, 0);
			}
			cancel_sent = TRUE;
		}
		for (i = 0; i < nb_slots; i++) {
			if ((slot[i].hProcess == NULL) || (WaitForSingleObject(slot[i].hProcess, 0) != WAIT_OBJECT_0))
				continue;
			// The worker exited => wait for its last messages to be processed
			WaitForSingleObject(slot[i].hReader, 10000);
			exit_code = 0;
			GetExitCodeProcess(slot[i].hProcess, &exit_code);
			// NB: Don't call uprintf() with the lock held, as it waits on the UI thread
			EnterCriticalSection(&slot[i].lock);
			crashed = !slot[i].done_received;
			if (crashed) {
				AppendSlotLog(&slot[i], "\r\n", 2);
				slot[i].error_status = RUFUS_ERROR(ERROR_GEN_FAILURE);
				slot[i].done_received = TRUE;
				PostSlotUpdate(i);
			}
			LeaveCriticalSection(&slot[i].lock);
			if (crashed)
				uprintf("[%s] Worker terminated unexpectedly (exit code 0x%08lX)", slot[i].tag, exit_code);
			CloseSlotHandles(&slot[i]);
			ReleaseLetters(i);
		}
	}

	if (vhd_mounted) {
		VhdUnmountImage();
		vhd_mounted = FALSE;
	}

	// Aggregate the results
	for (i = 0; i < nb_slots; i++) {
		if (IsCancelled(slot[i].error_status)) {
			nb_cancelled++;
		} else if (IS_ERROR(slot[i].error_status)) {
			if (nb_failed++ == 0)
				error_status = slot[i].error_status;
		}
	}
	if ((nb_failed == 0) && (nb_cancelled != 0))
		error_status = RUFUS_ERROR(ERROR_CANCELLED);
	ErrorStatus = error_status;
	PostMessage(hMainDialog, UM_FORMAT_COMPLETED, (WPARAM)TRUE, 0);
	return 0;
}

/*
 * Master side: UI
 */
static void GetDriveTag(DWORD device_num, char* tag, size_t tag_size)
{
	char letters[27] = { 0 };

	if (GetDriveLetters(device_num, letters) && (letters[0] != 0))
		safe_sprintf(tag, tag_size, "%c:", letters[0]);
	else
		safe_sprintf(tag, tag_size, "Disk %lu", device_num - DRIVE_INDEX_MIN);
}

static void SetSlotRow(int i)
{
	mufus_slot_t* s = &slot[i];
	char text[300];

	if (hStatusDlg == NULL || s->hBar == NULL)
		return;
	switch (s->state) {
	case MUFUS_STATE_PENDING:
		SetWindowTextU(s->hInfo, STR_STATE_PENDING);
		break;
	case MUFUS_STATE_RUNNING:
		SetWindowTextU(s->hInfo, (s->ui_info[0] != 0) ? s->ui_info : STR_STATE_STARTING);
		SendMessage(s->hBar, PBM_SETPOS, (WPARAM)s->ui_pos, 0);
		break;
	case MUFUS_STATE_SUCCESS:
		SetWindowTextU(s->hInfo, STR_STATE_SUCCESS);
		SendMessage(s->hBar, PBM_SETPOS, MAX_PROGRESS, 0);
		break;
	case MUFUS_STATE_CANCELLED:
		SetWindowTextU(s->hInfo, STR_STATE_CANCELLED);
		SendMessage(s->hBar, PBM_SETSTATE, (WPARAM)PBST_PAUSED, 0);
		break;
	case MUFUS_STATE_FAILED:
		static_sprintf(text, "✗ %s", StrError(s->error_status, FALSE));
		SetWindowTextU(s->hInfo, text);
		SendMessage(s->hBar, PBM_SETSTATE, (WPARAM)PBST_ERROR, 0);
		break;
	}
}

static void UpdateOverallProgress(void)
{
	int i, done = 0, success = 0;
	uint64_t total = 0;
	char text[128];

	if (nb_slots == 0)
		return;
	for (i = 0; i < nb_slots; i++) {
		if (slot[i].state >= MUFUS_STATE_SUCCESS) {
			done++;
			total += MAX_PROGRESS;
			if (slot[i].state == MUFUS_STATE_SUCCESS)
				success++;
		} else {
			total += slot[i].ui_pos;
		}
	}
	total /= nb_slots;
	SendMessage(hProgress, PBM_SETPOS, (WPARAM)total, 0);
	SetTaskbarProgressValue(total, MAX_PROGRESS);
	// Don't overwrite the "Cancelling" message from the main dialog
	if (multi_operation && !IsCancelled(ErrorStatus)) {
		static_sprintf(text, STR_PROGRESS_TEXT, nb_slots, (100.0f * total) / MAX_PROGRESS);
		SetWindowTextU(hProgress, text);
		InvalidateRect(hProgress, NULL, TRUE);
	}
	if (hStatusDlg != NULL) {
		if (multi_operation)
			static_sprintf(text, STR_SUMMARY_RUNNING, nb_slots, done);
		else
			static_sprintf(text, STR_SUMMARY_DONE, success, nb_slots);
		SetDlgItemTextU(hStatusDlg, IDC_MUFUS_SUMMARY, text);
	}
}

static void ProcessSlotUpdate(int i)
{
	mufus_slot_t* s = &slot[i];
	char* log = NULL;
	BOOL done;

	if ((i < 0) || (i >= nb_slots) || !s->initialized)
		return;
	EnterCriticalSection(&s->lock);
	log = s->log;
	s->log = NULL;
	s->log_len = 0;
	s->log_max = 0;
	done = s->done_received;
	s->ui_pos = s->pos;
	static_strcpy(s->ui_info, s->info);
	s->update_posted = FALSE;
	LeaveCriticalSection(&s->lock);
	if (log != NULL) {
		uprintfs(log);
		free(log);
	}
	if (done) {
		if (!IS_ERROR(s->error_status))
			s->state = MUFUS_STATE_SUCCESS;
		else if (IsCancelled(s->error_status))
			s->state = MUFUS_STATE_CANCELLED;
		else
			s->state = MUFUS_STATE_FAILED;
	} else if (s->state == MUFUS_STATE_PENDING) {
		s->state = MUFUS_STATE_RUNNING;
	}
	SetSlotRow(i);
	UpdateOverallProgress();
}

static void ProcessPrompts(void);

static void CALLBACK PromptTimer(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	KillTimer(hMainDialog, TID_MUFUS_PROMPT);
	ProcessPrompts();
}

static void ProcessPrompts(void)
{
	int i, j, r;
	char title[256];
	mufus_prompt_t* p;

	if (prompt_showing || (prompt_queue_len == 0))
		return;
	// Notification() relies on global data, so we must wait for any other dialog to be closed
	if (dialog_showing > 0) {
		SetTimer(hMainDialog, TID_MUFUS_PROMPT, 250, PromptTimer);
		return;
	}
	prompt_showing = TRUE;
	while (prompt_queue_len > 0) {
		i = prompt_queue[0];
		for (j = 1; j < prompt_queue_len; j++)
			prompt_queue[j - 1] = prompt_queue[j];
		prompt_queue_len--;
		p = prompt_data[i];
		prompt_data[i] = NULL;
		if (p == NULL)
			continue;
		static_sprintf(title, "[%s] %s", slot[i].tag, p->title);
		if (IsCancelled(ErrorStatus)) {
			r = CancelAnswer(p->type);
		} else {
			if (hStatusDlg != NULL)
				ShowWindow(hStatusDlg, SW_SHOWNOACTIVATE);
			r = Notification(p->type, title, "%s", p->message);
		}
		SendMsg(&slot[i].hToWorker,&slot[i].send_lock, MUFUS_MSG_PROMPT_REPLY, &r, sizeof(r));
		safe_free(p->title);
		safe_free(p->message);
		free(p);
	}
	prompt_showing = FALSE;
}

/*
 * Per-drive progress window
 */
static void PositionStatusDialog(HWND hDlg)
{
	RECT rc_main, rc_dlg, rc_work;
	HMONITOR monitor;
	MONITORINFO mi = { sizeof(MONITORINFO) };
	int w, h, x, y;

	GetWindowRect(hMainDialog, &rc_main);
	GetWindowRect(hDlg, &rc_dlg);
	w = rc_dlg.right - rc_dlg.left;
	h = rc_dlg.bottom - rc_dlg.top;
	monitor = MonitorFromWindow(hMainDialog, MONITOR_DEFAULTTONEAREST);
	GetMonitorInfo(monitor, &mi);
	rc_work = mi.rcWork;
	// Try the right side of the main window, then the left side, then just center on it
	if (rc_main.right + w <= rc_work.right)
		x = rc_main.right;
	else if (rc_main.left - w >= rc_work.left)
		x = rc_main.left - w;
	else
		x = rc_main.left + ((rc_main.right - rc_main.left) - w) / 2;
	y = rc_main.top;
	if (y + h > rc_work.bottom)
		y = rc_work.bottom - h;
	if (y < rc_work.top)
		y = rc_work.top;
	SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static HWND CreateRowControl(HWND hDlg, const wchar_t* class_name, DWORD style, int x, int y, int w, int h, int id)
{
	RECT rc = { x, y, x + w, y + h };
	HWND hCtrl;

	MapDialogRect(hDlg, &rc);
	hCtrl = CreateWindowExW(0, class_name, L"", WS_CHILD | WS_VISIBLE | style, rc.left, rc.top,
		rc.right - rc.left, rc.bottom - rc.top, hDlg, (HMENU)(uintptr_t)id, hMainInstance, NULL);
	if (hCtrl != NULL)
		SendMessage(hCtrl, WM_SETFONT, (WPARAM)GetWindowFont(hDlg), TRUE);
	return hCtrl;
}

static BOOL CALLBACK DestroyTooltipCallback(HWND hCtrl, LPARAM lParam)
{
	DestroyTooltip(hCtrl);
	return TRUE;
}

static INT_PTR CALLBACK StatusCallback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	int i;
	RECT rc;
	HWND hCtrl;

	switch (message) {
	case WM_INITDIALOG:
		// We are still within MyCreateDialog(), so set the handle now, for SetSlotRow()
		hStatusDlg = hDlg;
		SetDarkModeForDlg(hDlg);
		SetTitleBarIcon(hDlg);
		SetWindowTextU(hDlg, STR_STATUS_TITLE);
		for (i = 0; i < nb_slots; i++) {
			int y = 20 + i * MUFUS_ROW_HEIGHT;
			// NB: No SS_NOTIFY, as the dark mode code draws such static controls as hyperlinks
			slot[i].hName = CreateRowControl(hDlg, WC_STATICW, SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_NOPREFIX,
				8, y, 160, 9, IDC_MUFUS_ROW_BASE + 3 * i);
			slot[i].hInfo = CreateRowControl(hDlg, WC_STATICW, SS_RIGHT | SS_ENDELLIPSIS | SS_NOPREFIX,
				172, y, MUFUS_STATUS_WIDTH - 180, 9, IDC_MUFUS_ROW_BASE + 3 * i + 1);
			slot[i].hBar = CreateRowControl(hDlg, PROGRESS_CLASSW, PBS_SMOOTH,
				8, y + 10, MUFUS_STATUS_WIDTH - 16, 8, IDC_MUFUS_ROW_BASE + 3 * i + 2);
			SendMessage(slot[i].hBar, PBM_SETRANGE, 0, (MAX_PROGRESS << 16) & 0xFFFF0000);
			SubclassProgressBarControl(slot[i].hBar);
			SetWindowTextU(slot[i].hName, slot[i].name);
			// Static controls without SS_NOTIFY don't get mouse messages => show the full name over the bar
			CreateTooltipEx(hDlg, slot[i].hBar, slot[i].name, -1);
			SetSlotRow(i);
		}
		// Resize the dialog and move the Close button below the rows
		rc.left = 0;
		rc.top = 20 + nb_slots * MUFUS_ROW_HEIGHT + 4;
		rc.right = MUFUS_STATUS_WIDTH - 60;
		rc.bottom = rc.top + 14 + 7;
		MapDialogRect(hDlg, &rc);
		hCtrl = GetDlgItem(hDlg, IDCANCEL);
		SetWindowTextU(hCtrl, lmprintf(MSG_006));
		SetWindowPos(hCtrl, NULL, rc.right, rc.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
		{
			RECT rc_client, rc_window;
			GetClientRect(hDlg, &rc_client);
			GetWindowRect(hDlg, &rc_window);
			SetWindowPos(hDlg, NULL, 0, 0, rc_window.right - rc_window.left,
				(rc_window.bottom - rc_window.top) - rc_client.bottom + rc.bottom, SWP_NOMOVE | SWP_NOZORDER);
		}
		ResizeButtonHeight(hDlg, IDCANCEL);
		SetDarkModeForChild(hDlg);
		PositionStatusDialog(hDlg);
		UpdateOverallProgress();
		return (INT_PTR)TRUE;
	case WM_COMMAND:
		if ((LOWORD(wParam) == IDOK) || (LOWORD(wParam) == IDCANCEL)) {
			// Don't allow the window to be closed while the drives are being written
			if (!multi_operation)
				ShowWindow(hDlg, SW_HIDE);
			return (INT_PTR)TRUE;
		}
		break;
	case WM_CLOSE:
		if (!multi_operation)
			ShowWindow(hDlg, SW_HIDE);
		return (INT_PTR)TRUE;
	case WM_DESTROY:
		EnumChildWindows(hDlg, DestroyTooltipCallback, 0);
		break;
	}
	return (INT_PTR)FALSE;
}

static void CreateStatusDialog(void)
{
	int i;

	if (hStatusDlg != NULL)
		DestroyWindow(hStatusDlg);
	for (i = 0; i < nb_slots; i++) {
		slot[i].hName = NULL;
		slot[i].hInfo = NULL;
		slot[i].hBar = NULL;
	}
	hStatusDlg = MyCreateDialog(hMainInstance, IDD_MUFUS_STATUS, hMainDialog, StatusCallback);
	if (hStatusDlg != NULL) {
		EnableWindow(GetDlgItem(hStatusDlg, IDCANCEL), FALSE);
		ShowWindow(hStatusDlg, SW_SHOWNOACTIVATE);
	}
}

/*
 * Drive selection dialog
 */
static int CountChecked(HWND hList)
{
	int i, n = 0;

	for (i = 0; i < ListView_GetItemCount(hList); i++) {
		if (ListView_GetCheckState(hList, i))
			n++;
	}
	return n;
}

static void UpdateSelectionCount(HWND hDlg)
{
	char text[64];
	int n = CountChecked(GetDlgItem(hDlg, IDC_MUFUS_LIST));

	static_sprintf(text, STR_SELECT_COUNT, n, MUFUS_MAX_DRIVES);
	SetDlgItemTextU(hDlg, IDC_MUFUS_COUNT, text);
	EnableWindow(GetDlgItem(hDlg, IDOK), n > 0);
}

static char* picker_id[MAX_DRIVES];
static int picker_nb_rows = 0;

static INT_PTR CALLBACK SelectionCallback(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	static BOOL populating = FALSE;
	HWND hList;
	LVCOLUMNW lvc = { 0 };
	LVITEMW lvi = { 0 };
	NMLISTVIEW* nmlv;
	RECT rc;
	int i, j, n;
	wchar_t* wname;
	char text[256];

	switch (message) {
	case WM_INITDIALOG:
		SetDarkModeForDlg(hDlg);
		SetTitleBarIcon(hDlg);
		SetWindowTextU(hDlg, STR_SELECT_TITLE);
		static_sprintf(text, STR_SELECT_INFO, MUFUS_MAX_DRIVES);
		SetDlgItemTextU(hDlg, IDC_MUFUS_INFO, text);
		SetDlgItemTextU(hDlg, IDC_MUFUS_SELECT_ALL, STR_SELECT_ALL);
		SetDlgItemTextU(hDlg, IDC_MUFUS_SELECT_NONE, STR_SELECT_NONE);
		SetDlgItemTextU(hDlg, IDOK, "OK");
		SetDlgItemTextU(hDlg, IDCANCEL, lmprintf(MSG_007));
		hList = GetDlgItem(hDlg, IDC_MUFUS_LIST);
		ListView_SetExtendedListViewStyle(hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
		if (is_darkmode_enabled) {
			SetWindowTheme(hList, L"DarkMode_Explorer", NULL);
			ListView_SetBkColor(hList, DARKMODE_NORMAL_CONTROL_BACKGROUND_COLOR);
			ListView_SetTextBkColor(hList, DARKMODE_NORMAL_CONTROL_BACKGROUND_COLOR);
			ListView_SetTextColor(hList, DARKMODE_NORMAL_TEXT_COLOR);
		} else {
			SetWindowTheme(hList, L"Explorer", NULL);
		}
		GetClientRect(hList, &rc);
		lvc.mask = LVCF_WIDTH;
		lvc.cx = rc.right - rc.left - GetSystemMetrics(SM_CXVSCROLL);
		SendMessageW(hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&lvc);
		populating = TRUE;
		picker_nb_rows = 0;
		for (i = 0; (i < ComboBox_GetCount(hDeviceList)) && (i < MAX_DRIVES); i++) {
			wname = utf8_to_wchar(rufus_drive[i].display_name);
			picker_id[picker_nb_rows] = safe_strdup(STR_OR_EMPTY(rufus_drive[i].id));
			lvi.mask = LVIF_TEXT | LVIF_PARAM;
			lvi.iItem = i;
			lvi.pszText = (wname != NULL) ? wname : L"?";
			lvi.lParam = (LPARAM)picker_nb_rows++;
			n = (int)SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
			free(wname);
			// Pre-check the drives that are already selected
			for (j = 0; j < mufus_nb_selected; j++) {
				if (strcmp(mufus_selected_id[j], STR_OR_EMPTY(rufus_drive[i].id)) == 0)
					ListView_SetCheckState(hList, n, TRUE);
			}
		}
		populating = FALSE;
		UpdateSelectionCount(hDlg);
		ResizeButtonHeight(hDlg, IDOK);
		ResizeButtonHeight(hDlg, IDCANCEL);
		ResizeButtonHeight(hDlg, IDC_MUFUS_SELECT_ALL);
		ResizeButtonHeight(hDlg, IDC_MUFUS_SELECT_NONE);
		SetDarkModeForChild(hDlg);
		CenterDialog(hDlg, hMainDialog);
		return (INT_PTR)TRUE;
	case WM_NOTIFY:
		nmlv = (NMLISTVIEW*)lParam;
		if ((nmlv->hdr.idFrom != IDC_MUFUS_LIST) || populating)
			break;
		hList = nmlv->hdr.hwndFrom;
		if ((nmlv->hdr.code == LVN_ITEMCHANGING) && (nmlv->uChanged & LVIF_STATE) &&
			((nmlv->uNewState & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2)) &&
			((nmlv->uOldState & LVIS_STATEIMAGEMASK) != INDEXTOSTATEIMAGEMASK(2)) &&
			(CountChecked(hList) >= MUFUS_MAX_DRIVES)) {
			// Prevent the selection of more than the maximum number of drives
			static_sprintf(text, STR_SELECT_MAX, MUFUS_MAX_DRIVES);
			SetDlgItemTextU(hDlg, IDC_MUFUS_COUNT, text);
			MessageBeep(MB_ICONWARNING);
			SetWindowLongPtr(hDlg, DWLP_MSGRESULT, TRUE);
			return (INT_PTR)TRUE;
		}
		if ((nmlv->hdr.code == LVN_ITEMCHANGED) && (nmlv->uChanged & LVIF_STATE))
			UpdateSelectionCount(hDlg);
		break;
	case WM_COMMAND:
		hList = GetDlgItem(hDlg, IDC_MUFUS_LIST);
		switch (LOWORD(wParam)) {
		case IDC_MUFUS_SELECT_ALL:
			for (i = 0, n = 0; (i < ListView_GetItemCount(hList)) && (n < MUFUS_MAX_DRIVES); i++, n++)
				ListView_SetCheckState(hList, i, TRUE);
			UpdateSelectionCount(hDlg);
			return (INT_PTR)TRUE;
		case IDC_MUFUS_SELECT_NONE:
			for (i = 0; i < ListView_GetItemCount(hList); i++)
				ListView_SetCheckState(hList, i, FALSE);
			UpdateSelectionCount(hDlg);
			return (INT_PTR)TRUE;
		case IDOK:
			// Record the selection, in the order of the device list (i.e. by increasing size). The device
			// list may have been refreshed while we were open, so we match the drives on their device ID.
			for (i = 0, n = 0; (i < ListView_GetItemCount(hList)) && (n < MUFUS_MAX_DRIVES); i++) {
				if (!ListView_GetCheckState(hList, i))
					continue;
				lvi.mask = LVIF_PARAM;
				lvi.iItem = i;
				lvi.iSubItem = 0;
				if (!SendMessageW(hList, LVM_GETITEMW, 0, (LPARAM)&lvi) || (lvi.lParam < 0) ||
					(lvi.lParam >= picker_nb_rows) || (picker_id[lvi.lParam][0] == 0))
					continue;
				for (j = 0; (j < ComboBox_GetCount(hDeviceList)) && (j < MAX_DRIVES); j++) {
					if (strcmp(STR_OR_EMPTY(rufus_drive[j].id), picker_id[lvi.lParam]) == 0)
						break;
				}
				if ((j >= ComboBox_GetCount(hDeviceList)) || (j >= MAX_DRIVES)) {
					uprintf("Multi-drive mode: a selected drive is no longer present");
					continue;
				}
				mufus_selected[n] = rufus_drive[j].index;
				static_strcpy(mufus_selected_id[n], picker_id[lvi.lParam]);
				n++;
			}
			EndDialog(hDlg, n);
			return (INT_PTR)TRUE;
		case IDCANCEL:
			EndDialog(hDlg, -1);
			return (INT_PTR)TRUE;
		}
		break;
	case WM_DESTROY:
		for (i = 0; i < picker_nb_rows; i++)
			safe_free(picker_id[i]);
		picker_nb_rows = 0;
		break;
	}
	return (INT_PTR)FALSE;
}

/*
 * Multi-drive device selector, that takes the place of the device dropdown in multi-drive mode
 */
static LRESULT CALLBACK MultiDeviceProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
		SetFocus(hWnd);
		PostMessage(hMainDialog, UM_MUFUS_SELECT, 0, 0);
		return 0;
	case WM_KEYDOWN:
		if ((wParam == VK_F4) || (wParam == VK_DOWN) || (wParam == VK_UP) || (wParam == VK_SPACE) || (wParam == VK_RETURN)) {
			PostMessage(hMainDialog, UM_MUFUS_SELECT, 0, 0);
			return 0;
		}
		break;
	case WM_SYSKEYDOWN:
		if ((wParam == VK_DOWN) || (wParam == VK_UP)) {
			PostMessage(hMainDialog, UM_MUFUS_SELECT, 0, 0);
			return 0;
		}
		break;
	case WM_MOUSEWHEEL:
	case WM_CHAR:
		return 0;
	}
	return CallWindowProc(multi_device_original_proc, hWnd, message, wParam, lParam);
}

static void UpdateSelectionText(void)
{
	int i, idx;
	char text[512], tooltip[2048], *list;
	size_t len = 0;

	if (hMultiDevice == NULL)
		return;
	list = &text[0];
	text[0] = 0;
	tooltip[0] = 0;
	for (i = 0; i < mufus_nb_selected; i++) {
		idx = FindDriveIndex(mufus_selected[i]);
		if (idx < 0)
			continue;
		if (text[0] != 0)
			static_strcat(text, ", ");
		static_strcat(text, mufus_selected_tag[i]);
		len = strlen(tooltip);
		safe_sprintf(&tooltip[len], sizeof(tooltip) - len, "%s%s", (len == 0) ? "" : "\n", rufus_drive[idx].display_name);
	}
	{
		char display[600];
		static_sprintf(display, STR_MULTI_SELECTION, mufus_nb_selected, list);
		IGNORE_RETVAL(ComboBox_ResetContent(hMultiDevice));
		IGNORE_RETVAL(ComboBox_AddStringU(hMultiDevice, display));
		IGNORE_RETVAL(ComboBox_SetCurSel(hMultiDevice, 0));
	}
	CreateTooltip(hMultiDevice, tooltip, -1);
}

void MufusUpdateUI(void)
{
	RECT rc;

	if (mufus_worker || (hMainDialog == NULL))
		return;
	// EnableControls() is also called from other threads (e.g. the image scan thread), but the
	// tooltip of the drive selector must be created on the UI thread
	if (GetCurrentThreadId() != MainThreadId) {
		PostMessage(hMainDialog, UM_MUFUS_UPDATE_UI, 0, 0);
		return;
	}
	if (multi_mode && (ComboBox_GetCount(hDeviceList) <= 0)) {
		multi_mode = FALSE;
		mufus_nb_selected = 0;
	}
	if (hMultiDevice != NULL) {
		if (multi_mode) {
			// Take the exact place of the device dropdown
			GetWindowRect(hDeviceList, &rc);
			MapWindowPoints(NULL, hMainDialog, (POINT*)&rc, 2);
			SetWindowPos(hMultiDevice, hDeviceList, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOACTIVATE);
			UpdateSelectionText();
			EnableWindow(hMultiDevice, !op_in_progress);
			ShowWindow(hDeviceList, SW_HIDE);
			ShowWindow(hMultiDevice, SW_SHOW);
		} else {
			ShowWindow(hMultiDevice, SW_HIDE);
			ShowWindow(hDeviceList, SW_SHOW);
		}
	}
	if (hMultiDriveToolbar != NULL) {
		SendMessage(hMultiDriveToolbar, TB_CHECKBUTTON, (WPARAM)IDC_MULTI_DRIVE, (LPARAM)multi_mode);
		SendMessage(hMultiDriveToolbar, TB_ENABLEBUTTON, (WPARAM)IDC_MULTI_DRIVE,
			(LPARAM)(!op_in_progress && (ComboBox_GetCount(hDeviceList) > 0)));
	}
	// Saving a drive to an image is a single drive operation
	if (multi_mode)
		EnableWindow(hSaveToolbar, FALSE);
}

static void SelectPrimaryDrive(void)
{
	int idx;

	if (mufus_nb_selected == 0)
		return;
	// The selection is ordered by increasing size, so the first drive is the smallest one, which
	// is what the options (file system, cluster size, persistence, etc.) should be validated against.
	idx = FindDriveIndex(mufus_selected[0]);
	if ((idx >= 0) && (idx != ComboBox_GetCurSel(hDeviceList))) {
		IGNORE_RETVAL(ComboBox_SetCurSel(hDeviceList, idx));
		SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE << 16) | IDC_DEVICE, 0);
	}
}

static void SetMultiMode(BOOL enable)
{
	int i, n, idx;

	if (enable) {
		for (i = 0, n = 0; i < mufus_nb_selected; i++) {
			idx = FindDriveIndex(mufus_selected[i]);
			if ((idx < 0) || (strcmp(STR_OR_EMPTY(rufus_drive[idx].id), mufus_selected_id[i]) != 0))
				continue;
			mufus_selected[n] = mufus_selected[i];
			static_strcpy(mufus_selected_id[n], mufus_selected_id[i]);
			GetDriveTag(mufus_selected[n], mufus_selected_tag[n], sizeof(mufus_selected_tag[n]));
			n++;
		}
		mufus_nb_selected = n;
	}
	multi_mode = enable && (mufus_nb_selected >= 2);
	if (multi_mode) {
		SelectPrimaryDrive();
		uprintf("Multi-drive mode enabled for %d drives", mufus_nb_selected);
	} else if (mufus_nb_selected == 1) {
		// A single drive was picked => just select it in regular mode
		idx = FindDriveIndex(mufus_selected[0]);
		if ((idx >= 0) && (idx != ComboBox_GetCurSel(hDeviceList))) {
			IGNORE_RETVAL(ComboBox_SetCurSel(hDeviceList, idx));
			SendMessage(hMainDialog, WM_COMMAND, (CBN_SELCHANGE << 16) | IDC_DEVICE, 0);
		}
	}
	if (!multi_mode)
		mufus_nb_selected = 0;
	MufusUpdateUI();
	EnableControls(!op_in_progress, FALSE);
}

void MufusSelectDrives(void)
{
	int i, n;

	if (op_in_progress || (ComboBox_GetCount(hDeviceList) <= 0))
		return;
	if (!multi_mode) {
		// Start with the currently selected drive
		mufus_nb_selected = 0;
		i = ComboBox_GetCurSel(hDeviceList);
		if ((i >= 0) && (i < MAX_DRIVES)) {
			mufus_selected[0] = (DWORD)ComboBox_GetItemData(hDeviceList, i);
			static_strcpy(mufus_selected_id[0], STR_OR_EMPTY(rufus_drive[i].id));
			mufus_nb_selected = 1;
		}
	}
	n = (int)MyDialogBox(hMainInstance, IDD_MUFUS_SELECT, hMainDialog, SelectionCallback);
	if (n < 0) {
		// Cancelled => leave things as they were
		if (!multi_mode)
			mufus_nb_selected = 0;
		MufusUpdateUI();
		return;
	}
	mufus_nb_selected = n;
	SetMultiMode(TRUE);
}

void MufusToggleMultiMode(void)
{
	if (op_in_progress)
		return;
	if (multi_mode) {
		mufus_nb_selected = 0;
		SetMultiMode(FALSE);
		uprintf("Multi-drive mode disabled");
	} else {
		MufusSelectDrives();
	}
}

/*
 * Called by GetDevices() after the drives have been enumerated, but before the device list selection
 * is set, so that we can drop the drives that went away. Returns the device that should be selected.
 */
DWORD MufusOnDevicesRefreshed(DWORD devnum, int num_drives)
{
	int i, j, n = 0;

	if (mufus_worker || !multi_mode)
		return devnum;
	for (i = 0; i < mufus_nb_selected; i++) {
		// Match drives on their device instance ID, as their disk number may have changed
		// (e.g. if the device was cycled), and fall back to the disk number if we have no ID.
		for (j = 0; j < num_drives; j++) {
			if ((mufus_selected_id[i][0] != 0) ? (strcmp(STR_OR_EMPTY(rufus_drive[j].id), mufus_selected_id[i]) == 0) :
				(rufus_drive[j].index == mufus_selected[i]))
				break;
		}
		if (j >= num_drives) {
			uprintf("Multi-drive mode: drive 0x%02lX is no longer present", mufus_selected[i]);
			continue;
		}
		mufus_selected[n] = rufus_drive[j].index;
		static_strcpy(mufus_selected_id[n], mufus_selected_id[i]);
		// The letters may have changed (e.g. after we formatted the drive)
		GetDriveTag(mufus_selected[n], mufus_selected_tag[n], sizeof(mufus_selected_tag[n]));
		n++;
	}
	mufus_nb_selected = n;
	if (n < 2) {
		multi_mode = FALSE;
		uprintf("Multi-drive mode disabled");
		if (n == 1)
			devnum = mufus_selected[0];
		mufus_nb_selected = 0;
		// Don't rely on the device selection to restore the regular device list, as no drive may be left
		MufusUpdateUI();
		return devnum;
	}
	// Keep the smallest drive as the reference
	for (j = 0; j < num_drives; j++) {
		for (i = 0; i < mufus_nb_selected; i++) {
			if (rufus_drive[j].index == mufus_selected[i])
				return mufus_selected[i];
		}
	}
	return devnum;
}

/*
 * Validation of all the selected drives, and confirmation, in lieu of the single drive
 * checks that the master performs when processing UM_FORMAT_START.
 */
BOOL MufusConfirmFormat(void)
{
	RUFUS_DRIVE_INFO saved;
	BOOL r = FALSE;
	int i, idx, nb_multi_part = 0;
	uint64_t max_size = 0, mbr_overflow = 0;
	DWORD bad_sector_size = 0;
	ULONG cluster_size;
	char fs_name[32], *list = NULL, title[128];
	size_t list_size = 0, len;
	BOOL is_windows_to_go = (image_options & IMOP_WINTOGO) && (ComboBox_GetCurItemData(hImageOption) == IMOP_WIN_TO_GO);

	if (mufus_nb_selected < 2)
		return FALSE;
	cluster_size = (ULONG)ComboBox_GetCurItemData(hClusterSize);
	memcpy(&saved, &SelectedDrive, sizeof(saved));
	list_size = mufus_nb_selected * 300;
	list = (char*)calloc(1, list_size);
	if (list == NULL)
		goto out;
	for (i = 0; i < mufus_nb_selected; i++) {
		idx = FindDriveIndex(mufus_selected[i]);
		if (idx < 0) {
			Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_042), STR_DRIVE_GONE, "?");
			goto out;
		}
		len = strlen(list);
		safe_sprintf(&list[len], list_size - len, "• %s\n", rufus_drive[idx].display_name);
		memset(&SelectedDrive, 0, sizeof(SelectedDrive));
		SelectedDrive.DeviceNumber = mufus_selected[i];
		GetDrivePartitionData(SelectedDrive.DeviceNumber, fs_name, sizeof(fs_name), TRUE);
		if (SelectedDrive.DiskSize == 0) {
			Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_042), STR_DRIVE_GONE, rufus_drive[idx].display_name);
			goto out;
		}
		ComputeClusterSizes();
		if (!zero_drive && (boot_type == BT_IMAGE) && (image_path != NULL)) {
			if (IsSourceImageLocatedOnTargetDrive(mufus_selected[i])) {
				Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_358), "%s\n\n%s", lmprintf(MSG_359), rufus_drive[idx].display_name);
				goto out;
			}
			if (size_check && (img_report.projected_size > ((uint64_t)SelectedDrive.DiskSize + 4 * KB))) {
				Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_088), "%s\n\n%s", lmprintf(MSG_089), rufus_drive[idx].display_name);
				goto out;
			}
		}
		if (!zero_drive && !write_as_image && (fs_type >= 0) && (fs_type < FS_MAX)) {
			if (SelectedDrive.ClusterSize[fs_type].Allowed == 0) {
				Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_092), STR_DRIVE_FS, FileSystemLabel[fs_type], rufus_drive[idx].display_name);
				goto out;
			}
			if ((cluster_size >= 0x200) && (cluster_size != saved.ClusterSize[fs_type].Default) &&
				(saved.ClusterSize[fs_type].Allowed & cluster_size) &&
				(SelectedDrive.ClusterSize[fs_type].Allowed != SINGLE_CLUSTERSIZE_DEFAULT) &&
				!(SelectedDrive.ClusterSize[fs_type].Allowed & cluster_size)) {
				Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_092), STR_DRIVE_CLUSTER, rufus_drive[idx].display_name);
				goto out;
			}
		}
		if (!zero_drive && (persistence_size != 0) && (boot_type == BT_IMAGE) && !write_as_image &&
			(persistence_size > SelectedDrive.DiskSize - PERCENTAGE(PROJECTED_SIZE_RATIO, img_report.projected_size))) {
			Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_092), STR_DRIVE_PERSISTENCE, rufus_drive[idx].display_name);
			goto out;
		}
		if (!zero_drive && is_windows_to_go && !write_as_image && (SelectedDrive.MediaType != FixedMedia) && (target_type == TT_UEFI) &&
			(partition_type == PARTITION_STYLE_GPT) && (WindowsVersion.BuildNumber < 15000)) {
			Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_190), lmprintf(MSG_198));
			goto out;
		}
		if ((partition_type == PARTITION_STYLE_MBR) && (SelectedDrive.DiskSize > 2 * TB))
			mbr_overflow = max(mbr_overflow, (uint64_t)(SelectedDrive.DiskSize - 2 * TB));
		max_size = max(max_size, (uint64_t)SelectedDrive.DiskSize);
		if (SelectedDrive.nPartitions > 1)
			nb_multi_part++;
		if (!zero_drive && (boot_type != BT_NON_BOOTABLE) && (SelectedDrive.SectorSize != 512))
			bad_sector_size = SelectedDrive.SectorSize;
	}
	memcpy(&SelectedDrive, &saved, sizeof(saved));

	// Same set of prompts as the single drive ones, applied to all the drives
	if ((mbr_overflow != 0) && (Notification(MB_YESNO | MB_ICONWARNING, lmprintf(MSG_128, "MBR"),
		lmprintf(MSG_134, SizeToHumanReadable(mbr_overflow, FALSE, FALSE))) != IDYES))
		goto out;
	if (!zero_drive && (fs_type == FS_UDF)) {
		dur_secs = (uint32_t)(((double)max_size) / 1073741824.0f / UDF_FORMAT_SPEED);
		if (dur_secs > UDF_FORMAT_WARN) {
			dur_mins = dur_secs / 60;
			dur_secs -= dur_mins * 60;
			Notification(MB_OK | MB_ICONINFORMATION, lmprintf(MSG_113), lmprintf(MSG_112, dur_mins, dur_secs));
		} else {
			dur_secs = 0;
			dur_mins = 0;
		}
	}
	// The conflicting process search covers the reference drive (searching all the drives would take
	// too long). Workers search for conflicting processes on their own drive, if they need to.
	PrintStatus(0, MSG_278);
	if (GetProcessSearch(SEARCH_PROCESS_TIMEOUT, 0x06, TRUE)) {
		ComboBox_GetTextU(hDeviceList, title, sizeof(title));
		if (Notification(MB_ICONWARNING | MB_YESNO, title, lmprintf(MSG_132)) != IDYES)
			goto out;
	}
	PrintStatus(0, MSG_142);
	if (Notification(MB_OKCANCEL | MB_ICONWARNING, STR_CONFIRM_TITLE, STR_CONFIRM_TEXT, mufus_nb_selected, list) != IDOK)
		goto out;
	if ((nb_multi_part != 0) && (Notification(MB_OKCANCEL | MB_ICONWARNING, lmprintf(MSG_094), lmprintf(MSG_093)) != IDOK))
		goto out;
	if ((bad_sector_size != 0) && (Notification(MB_OKCANCEL | MB_ICONWARNING, lmprintf(MSG_197),
		lmprintf(MSG_196, bad_sector_size)) != IDOK))
		goto out;
	r = TRUE;

out:
	memcpy(&SelectedDrive, &saved, sizeof(saved));
	free(list);
	return r;
}

// Release the resources of the previous operation, if any
static void ResetSlots(void)
{
	int i;

	for (i = 0; i < MUFUS_MAX_DRIVES; i++) {
		if (slot[i].unattend_copy[0] != 0)
			DeleteFileU(slot[i].unattend_copy);
		safe_free(slot[i].log);
		safe_free(slot[i].job);
		if (slot[i].initialized) {
			DeleteCriticalSection(&slot[i].lock);
			DeleteCriticalSection(&slot[i].send_lock);
		}
		memset(&slot[i], 0, sizeof(slot[i]));
	}
	for (i = 0; i < MUFUS_MAX_DRIVES; i++) {
		if (prompt_data[i] != NULL) {
			safe_free(prompt_data[i]->title);
			safe_free(prompt_data[i]->message);
			safe_free(prompt_data[i]);
		}
	}
	prompt_queue_len = 0;
	nb_slots = 0;
}

/*
 * Start writing to all the selected drives. Must be called from the UI thread.
 * Returns the handle of a supervisor thread, which takes the place of FormatThread.
 */
HANDLE MufusStartFormat(void)
{
	int i, idx;
	size_t job_size = 0;
	mufus_job_t* job = NULL;
	char letters[27];
	HANDLE hThread = NULL;

	if (hStatusDlg != NULL) {
		DestroyWindow(hStatusDlg);
		hStatusDlg = NULL;
	}
	ResetSlots();
	if (!OpenLetterTable(GetCurrentProcessId(), TRUE)) {
		uprintf("Could not create the drive letters table: %s", WindowsErrorString());
		return NULL;
	}
	memset(letter_owner, 0, 32);

	// A VHD can only be mounted once, so mount it here, for all the workers to use
	if ((boot_type == BT_IMAGE) && write_as_image && (image_path != NULL) &&
		((img_report.compression_type == IMG_COMPRESSION_VHD) || (img_report.compression_type == IMG_COMPRESSION_VHDX))) {
		char* path = VhdMountImageAndGetSize(image_path, &vhd_size);
		if ((path == NULL) || (vhd_size == 0)) {
			uprintf("Could not mount VHD source image");
			CloseLetterTable();
			return NULL;
		}
		static_strcpy(vhd_physical_path, path);
		vhd_mounted = TRUE;
	}

	job = BuildJob(&job_size);
	if (job == NULL)
		goto out;

	// Set up the slots
	nb_slots = 0;
	for (i = 0; i < mufus_nb_selected; i++) {
		mufus_slot_t* s;
		idx = FindDriveIndex(mufus_selected[i]);
		if (idx < 0) {
			uprintf("Drive 0x%02lX is no longer present - skipping", mufus_selected[i]);
			continue;
		}
		s = &slot[nb_slots];
		InitializeCriticalSection(&s->lock);
		InitializeCriticalSection(&s->send_lock);
		s->initialized = TRUE;
		s->device_num = mufus_selected[i];
		s->log_at_line_start = TRUE;
		s->state = MUFUS_STATE_PENDING;
		static_strcpy(s->id, STR_OR_EMPTY(rufus_drive[idx].id));
		static_strcpy(s->name, STR_OR_EMPTY(rufus_drive[idx].display_name));
		static_strcpy(s->tag, mufus_selected_tag[i]);
		// Reserve the existing letters of the drive for its worker
		if (GetDriveLetters(s->device_num, letters)) {
			char* l;
			for (l = letters; *l != 0; l++) {
				if ((toupper(*l) >= 'A') && (toupper(*l) <= 'Z'))
					letter_owner[toupper(*l) - 'A'] = (uint8_t)(nb_slots + 1);
			}
		}
		s->job = (mufus_job_t*)malloc(job_size);
		if (s->job == NULL)
			goto out;
		s->job_size = job_size;
		memcpy(s->job, job, job_size);
		s->job->slot = nb_slots;
		s->job->device_num = s->device_num;
		s->job->drive_index = rufus_drive[idx].index;
		s->job->drive_port = rufus_drive[idx].port;
		s->job->drive_size = rufus_drive[idx].size;
		static_strcpy(s->job->drive_id, STR_OR_EMPTY(rufus_drive[idx].id));
		static_strcpy(s->job->drive_name, STR_OR_EMPTY(rufus_drive[idx].name));
		static_strcpy(s->job->drive_display_name, STR_OR_EMPTY(rufus_drive[idx].display_name));
		static_strcpy(s->job->drive_label, STR_OR_EMPTY(rufus_drive[idx].label));
		static_strcpy(s->job->drive_hub, STR_OR_EMPTY(rufus_drive[idx].hub));
		static_strcpy(s->job->device_text, STR_OR_EMPTY(rufus_drive[idx].display_name));
		static_sprintf(s->job->hive_name, "MUFUS_OFFLINE_HIVE_%lX_%d", GetCurrentProcessId(), nb_slots);
		// ApplyWindowsCustomization() modifies the unattend.xml it is given, so each worker needs its own copy
		if (unattend_xml_path != NULL) {
			if ((GetTempFileNameU(temp_dir, APPLICATION_NAME, 0, s->unattend_copy) == 0) ||
				!CopyFileU(unattend_xml_path, s->unattend_copy, FALSE)) {
				uprintf("Could not duplicate '%s': %s", unattend_xml_path, WindowsErrorString());
				goto out;
			}
			static_strcpy(s->job->unattend_xml_path, s->unattend_copy);
		}
		nb_slots++;
	}
	if (nb_slots == 0)
		goto out;

	uprintf("\r\nMulti-drive operation on %d drives:", nb_slots);
	for (i = 0; i < nb_slots; i++)
		uprintf("● [%s] %s", slot[i].tag, slot[i].name);
	multi_operation = TRUE;
	multi_start_time = GetTickCount64();
	CreateStatusDialog();
	UpdateOverallProgress();
	hThread = CreateThread(NULL, 0, SupervisorThread, NULL, 0, NULL);
	if (hThread == NULL)
		multi_operation = FALSE;

out:
	free(job);
	if (hThread == NULL) {
		uprintf("Could not start the multi-drive operation");
		if (hStatusDlg != NULL)
			DestroyWindow(hStatusDlg);
		hStatusDlg = NULL;
		ResetSlots();
		CloseLetterTable();
		if (vhd_mounted) {
			VhdUnmountImage();
			vhd_mounted = FALSE;
		}
	}
	return hThread;
}

BOOL MufusIsFormatting(void)
{
	return multi_operation;
}

/*
 * Called on the UI thread when the multi-drive operation is over.
 * Returns TRUE if this was a multi-drive operation.
 */
BOOL MufusFormatCompleted(void)
{
	int i;
	BOOL was_multi = multi_operation;

	multi_operation = FALSE;
	if (!was_multi) {
		// Leave the results of the last multi-drive operation (and the progress bar) alone
		CloseLetterTable();
		return FALSE;
	}
	KillTimer(hMainDialog, TID_MUFUS_PROMPT);
	for (i = 0; i < MUFUS_MAX_DRIVES; i++) {
		if (prompt_data[i] != NULL) {
			safe_free(prompt_data[i]->title);
			safe_free(prompt_data[i]->message);
			safe_free(prompt_data[i]);
		}
	}
	prompt_queue_len = 0;
	for (i = 0; i < nb_slots; i++) {
		// Process any update we haven't seen yet
		ProcessSlotUpdate(i);
		if (slot[i].state < MUFUS_STATE_SUCCESS) {
			slot[i].state = MUFUS_STATE_CANCELLED;
			SetSlotRow(i);
		}
		if (slot[i].unattend_copy[0] != 0) {
			DeleteFileU(slot[i].unattend_copy);
			slot[i].unattend_copy[0] = 0;
		}
		safe_free(slot[i].job);
	}
	CloseLetterTable();

	UpdateOverallProgress();
	if (hStatusDlg != NULL)
		EnableWindow(GetDlgItem(hStatusDlg, IDCANCEL), TRUE);
	uprintf("\r\nMulti-drive operation results (%llu seconds):", (GetTickCount64() - multi_start_time) / 1000);
	for (i = 0; i < nb_slots; i++) {
		uprintf("● [%s] %s: %s", slot[i].tag, slot[i].name, (slot[i].state == MUFUS_STATE_SUCCESS) ? "Success" :
			((slot[i].state == MUFUS_STATE_CANCELLED) ? "Cancelled" : StrError(slot[i].error_status, TRUE)));
	}
	return TRUE;
}

// Display the drives that failed
void MufusShowResults(void)
{
	int i, nb_failed = 0;
	char *list = NULL, title[64];
	size_t list_size, len;

	list_size = nb_slots * 400 + 1;
	list = (char*)calloc(1, list_size);
	if (list == NULL)
		return;
	for (i = 0; i < nb_slots; i++) {
		if (slot[i].state != MUFUS_STATE_FAILED)
			continue;
		nb_failed++;
		len = strlen(list);
		safe_sprintf(&list[len], list_size - len, "✗ %s\n    %s\n", slot[i].name, StrError(slot[i].error_status, FALSE));
	}
	if (nb_failed != 0) {
		static_sprintf(title, STR_RESULT_TITLE, nb_failed, nb_slots);
		Notification(MB_ICONERROR | MB_CLOSE, title, STR_RESULT_TEXT, list);
	}
	free(list);
}

/*
 * Main dialog integration
 */

/*
 * Rufus and Mufus both alter system-wide settings that they restore on exit (AutoMount and the
 * NoDriveTypeAutorun policy) and use the same offline registry hive name, so they must not run at
 * the same time. Since Rufus doesn't know about Mufus, Mufus also holds the Rufus mutex, which both
 * prevents Rufus from starting while Mufus runs, and Mufus from starting while Rufus runs.
 * Returns the mutex handle, or NULL if Rufus is running.
 */
HANDLE MufusAcquireRufusMutex(int wait)
{
	HANDLE mutex = CreateMutexA(NULL, TRUE, "Global/Rufus");

	for (; (wait > 0) && (mutex != NULL) && (GetLastError() == ERROR_ALREADY_EXISTS); wait--) {
		CloseHandle(mutex);
		Sleep(100);
		mutex = CreateMutexA(NULL, TRUE, "Global/Rufus");
	}
	if ((mutex != NULL) && (GetLastError() == ERROR_ALREADY_EXISTS)) {
		CloseHandle(mutex);
		mutex = NULL;
	}
	return mutex;
}

void MufusInit(HWND hDlg)
{
	INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES };
	RECT rc;

	if (mufus_worker)
		return;
	InitCommonControlsEx(&icc);
	GetWindowRect(hDeviceList, &rc);
	MapWindowPoints(NULL, hDlg, (POINT*)&rc, 2);
	hMultiDevice = CreateWindowExW(0, WC_COMBOBOXW, NULL, WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
		rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, hDlg, (HMENU)IDC_MULTI_DEVICE, hMainInstance, NULL);
	if (hMultiDevice == NULL)
		return;
	SendMessage(hMultiDevice, WM_SETFONT, (WPARAM)GetWindowFont(hDeviceList), TRUE);
	multi_device_original_proc = (WNDPROC)SetWindowLongPtr(hMultiDevice, GWLP_WNDPROC, (LONG_PTR)MultiDeviceProc);
	SetAccessibleName(hMultiDevice, "Selected drives");
	if (is_darkmode_enabled)
		SetWindowTheme(hMultiDevice, L"DarkMode_CFD", NULL);
}

// Create the multi-drive toggle button, in the same style as the other small toolbar buttons
void MufusCreateToolbar(HWND hDlg)
{
	HICON hIcon;
	TBBUTTON tbToolbarButtons[1];
	unsigned char* buffer;
	DWORD bufsize;
	int icon_offset = 0, i16 = GetSystemMetrics(SM_CXSMICON);

	if (mufus_worker)
		return;
	if (i16 >= 28)
		icon_offset = 20;
	else if (i16 >= 20)
		icon_offset = 10;

	hMultiDriveToolbar = CreateWindowEx(0, TOOLBARCLASSNAME, NULL, TOOLBAR_STYLE,
		0, 0, 0, 0, hDlg, (HMENU)IDC_MULTI_DRIVE_TOOLBAR, hMainInstance, NULL);
	hMultiImageList = ImageList_Create(i16, i16, ILC_COLOR32 | ILC_HIGHQUALITYSCALE | ILC_MIRROR, 1, 0);
	buffer = GetResource(hMainInstance, MAKEINTRESOURCEA(IDI_MULTI_16 + icon_offset), _RT_RCDATA, "multi icon", &bufsize, FALSE);
	hIcon = CreateIconFromResourceEx(buffer, bufsize, TRUE, 0x30000, 0, 0, 0);
	ChangeIconColor(&hIcon, 0);
	ImageList_AddIcon(hMultiImageList, hIcon);
	DestroyIcon(hIcon);
	SendMessage(hMultiDriveToolbar, TB_SETIMAGELIST, (WPARAM)0, (LPARAM)hMultiImageList);
	SendMessage(hMultiDriveToolbar, TB_BUTTONSTRUCTSIZE, (WPARAM)sizeof(TBBUTTON), 0);
	memset(tbToolbarButtons, 0, sizeof(TBBUTTON));
	tbToolbarButtons[0].idCommand = IDC_MULTI_DRIVE;
	tbToolbarButtons[0].fsStyle = BTNS_AUTOSIZE | BTNS_CHECK;
	tbToolbarButtons[0].fsState = TBSTATE_ENABLED;
	tbToolbarButtons[0].iBitmap = 0;
	SendMessage(hMultiDriveToolbar, TB_ADDBUTTONS, (WPARAM)1, (LPARAM)&tbToolbarButtons);
	SetAccessibleName(hMultiDriveToolbar, "Multi-drive mode");
}

HWND MufusGetToolbar(void)
{
	return hMultiDriveToolbar;
}

const char* MufusGetTooltip(void)
{
	return STR_MULTI_TOOLTIP;
}

void MufusExit(void)
{
	if (mufus_worker)
		return;
	if (hStatusDlg != NULL)
		DestroyWindow(hStatusDlg);
	hStatusDlg = NULL;
	ResetSlots();
	if (hMultiImageList != NULL)
		ImageList_Destroy(hMultiImageList);
	hMultiImageList = NULL;
	// These are destroyed along with the main dialog
	hMultiDevice = NULL;
	hMultiDriveToolbar = NULL;
	CloseLetterTable();
}

INT_PTR MufusHandleMessage(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	int i;

	switch (message) {
	case UM_MUFUS_UPDATE:
		ProcessSlotUpdate((int)wParam);
		return (INT_PTR)TRUE;
	case UM_MUFUS_PROMPT:
		i = (int)wParam;
		if ((i < 0) || (i >= nb_slots) || (lParam == 0))
			return (INT_PTR)TRUE;
		if (prompt_data[i] != NULL) {
			// Should not happen as workers only issue one prompt at a time
			mufus_prompt_t* p = (mufus_prompt_t*)lParam;
			safe_free(p->title);
			safe_free(p->message);
			free(p);
			return (INT_PTR)TRUE;
		}
		prompt_data[i] = (mufus_prompt_t*)lParam;
		if (prompt_queue_len < ARRAYSIZE(prompt_queue))
			prompt_queue[prompt_queue_len++] = i;
		ProcessPrompts();
		return (INT_PTR)TRUE;
	case UM_MUFUS_SELECT:
		MufusSelectDrives();
		return (INT_PTR)TRUE;
	case UM_MUFUS_UPDATE_UI:
		MufusUpdateUI();
		return (INT_PTR)TRUE;
	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_MULTI_DRIVE) {
			MufusToggleMultiMode();
			return (INT_PTR)TRUE;
		}
		break;
	}
	return (INT_PTR)FALSE;
}
