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

#include <windows.h>
#include <stdint.h>

#pragma once

/*
 * How multi-drive writing works:
 *
 * The UI process (the "master") performs all of the regular Rufus pre-format work
 * exactly once (option validation, prompts, downloads, unattend.xml creation, ...).
 * Then, instead of starting a single FormatThread, it starts one hidden copy of its
 * own executable (a "worker") per selected drive. Each worker receives a snapshot of
 * the master's state through a pipe, rebuilds an identical (but invisible) main
 * dialog and runs the unmodified Rufus FormatThread against its own drive. All log,
 * progress, status and prompt output from a worker is relayed back to the master.
 *
 * Process isolation is deliberate: Rufus keeps most of its state in globals and
 * static buffers, so running several FormatThreads within the same process would
 * not be safe. With one process per drive, every drive gets its own copy of that
 * state and the format code can be used as is.
 */

#define MUFUS_MAX_DRIVES            10
#define MUFUS_WORKER_ARG            "--mufus-worker"
#define MUFUS_STR_RUFUS_RUNNING     "Rufus is running.\nPlease close Rufus before running Mufus, as both applications cannot run at the same time."

/* Messages exchanged between master and workers over the pipes */
enum mufus_msg_type {
	MUFUS_MSG_JOB = 1,              // master → worker: the job description
	MUFUS_MSG_CANCEL,               // master → worker: cancel the operation
	MUFUS_MSG_PROMPT_REPLY,         // master → worker: answer to a prompt
	MUFUS_MSG_LOG = 0x100,          // worker → master: log text (UTF-8)
	MUFUS_MSG_PROGRESS,             // worker → master: progress position (0 to MAX_PROGRESS)
	MUFUS_MSG_INFO,                 // worker → master: info text (displayed on the progress bar)
	MUFUS_MSG_STATUS,               // worker → master: status bar text
	MUFUS_MSG_PROMPT,               // worker → master: request for a user prompt
	MUFUS_MSG_DONE,                 // worker → master: operation completed (with ErrorStatus)
};

/* Result states of a drive, from the master's point of view */
enum mufus_drive_state {
	MUFUS_STATE_PENDING = 0,
	MUFUS_STATE_RUNNING,
	MUFUS_STATE_SUCCESS,
	MUFUS_STATE_FAILED,
	MUFUS_STATE_CANCELLED,
};

/* Multi-drive selection (master) */
extern BOOL multi_mode;
extern BOOL mufus_worker;
extern int mufus_nb_selected;
extern DWORD mufus_selected[MUFUS_MAX_DRIVES];

/* Master side */
extern HANDLE MufusAcquireRufusMutex(int wait);
extern void MufusInit(HWND hDlg);
extern void MufusCreateToolbar(HWND hDlg);
extern HWND MufusGetToolbar(void);
extern const char* MufusGetTooltip(void);
extern void MufusExit(void);
extern void MufusToggleMultiMode(void);
extern void MufusSelectDrives(void);
extern DWORD MufusOnDevicesRefreshed(DWORD devnum, int num_drives);
extern void MufusUpdateUI(void);
extern BOOL MufusConfirmFormat(void);
extern HANDLE MufusStartFormat(void);
extern BOOL MufusIsFormatting(void);
extern BOOL MufusFormatCompleted(void);
extern void MufusShowResults(void);
extern INT_PTR MufusHandleMessage(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

/* Worker side */
extern BOOL MufusParseWorkerArgs(void);
extern int MufusWorkerMain(void);
extern void MufusWorkerLog(const char* str);
extern void MufusWorkerLogW(const wchar_t* wstr);
extern void MufusWorkerProgress(int pos);
extern void MufusWorkerInfo(BOOL info, const char* msg);
extern int MufusWorkerPrompt(int type, const char* title, const char* message);
extern char MufusGetUnusedDriveLetter(void);
extern const char* MufusHiveName(const char* default_name);
extern char* MufusGetVhdSource(uint64_t* disk_size);
extern void MufusBeginCycle(void);
extern BOOL MufusEndCycle(DWORD DriveIndex);
extern void MufusRefreshLayout(void);
extern BOOL MufusDriveHasNoPartitions(DWORD DriveIndex);
extern void MufusFormatInUse(DWORD DriveIndex, uint64_t PartitionOffset);
