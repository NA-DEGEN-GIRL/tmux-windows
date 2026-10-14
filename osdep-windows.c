/*
 * OS-dependent functions for Windows.
 * Implements osdep_get_name(), osdep_get_cwd(), osdep_event_init().
 */

#ifdef _WIN32

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>

/* Deepest process chain followed when looking for the foreground process. */
#define OSDEP_MAX_DEPTH 16

/*
 * Find the pane process for a pane fd. The fd is the socket bridged from
 * ConPTY, so there is no terminal to ask for its foreground process group.
 */
static DWORD
osdep_pane_pid(int fd)
{
	struct window_pane	*wp;

	if (fd == -1)
		return (0);
	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		if (wp->fd == fd && wp->pid > 0)
			return ((DWORD)wp->pid);
	}
	return (0);
}

static int
osdep_process_time(DWORD pid, ULONGLONG *created)
{
	HANDLE		h;
	FILETIME	ct, et, kt, ut;
	int		ok;

	h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (h == NULL)
		return (0);
	ok = GetProcessTimes(h, &ct, &et, &kt, &ut);
	CloseHandle(h);
	if (!ok)
		return (0);
	*created = ((ULONGLONG)ct.dwHighDateTime << 32) | ct.dwLowDateTime;
	return (1);
}

/*
 * Approximate the foreground process of a pane: starting at the pane process,
 * repeatedly follow the most recently created live child. Children must have
 * been created after their parent so a reused parent PID is not followed.
 * Fills chain with the processes visited, pane process first.
 */
static u_int
osdep_process_chain(DWORD root, DWORD *chain, u_int size,
    wchar_t name[MAX_PATH])
{
	HANDLE		 snap;
	PROCESSENTRY32W	*pe = NULL;
	u_int		 n = 0, count = 0, alloc = 0, i, best;
	ULONGLONG	 parent_time, best_time, t;
	PROCESSENTRY32W	 entry;

	if (root == 0 || size == 0)
		return (0);
	if (!osdep_process_time(root, &parent_time))
		return (0);

	snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return (0);
	entry.dwSize = sizeof entry;
	if (Process32FirstW(snap, &entry)) {
		do {
			if (count == alloc) {
				alloc = alloc == 0 ? 256 : alloc * 2;
				pe = xreallocarray(pe, alloc, sizeof *pe);
			}
			pe[count++] = entry;
		} while (Process32NextW(snap, &entry));
	}
	CloseHandle(snap);

	name[0] = L'\0';
	for (i = 0; i < count; i++) {
		if (pe[i].th32ProcessID == root) {
			wcsncpy_s(name, MAX_PATH, pe[i].szExeFile, _TRUNCATE);
			break;
		}
	}
	chain[n++] = root;

	while (n < size && n < OSDEP_MAX_DEPTH) {
		best = count;
		best_time = 0;
		for (i = 0; i < count; i++) {
			if (pe[i].th32ParentProcessID != chain[n - 1] ||
			    pe[i].th32ProcessID == chain[n - 1])
				continue;
			if (_wcsicmp(pe[i].szExeFile, L"conhost.exe") == 0 ||
			    _wcsicmp(pe[i].szExeFile, L"OpenConsole.exe") == 0)
				continue;
			if (!osdep_process_time(pe[i].th32ProcessID, &t))
				continue;
			if (t < parent_time)
				continue;
			if (best == count || t > best_time) {
				best = i;
				best_time = t;
			}
		}
		if (best == count)
			break;
		chain[n++] = pe[best].th32ProcessID;
		wcsncpy_s(name, MAX_PATH, pe[best].szExeFile, _TRUNCATE);
		parent_time = best_time;
	}

	free(pe);
	return (n);
}

/*
 * Get the name of the foreground process in a pane, without any directory or
 * .exe suffix so names match what Unix tmux reports (bash, vim, node, ...).
 */
char *
osdep_get_name(int fd, __unused char *tty)
{
	DWORD	 chain[OSDEP_MAX_DEPTH];
	wchar_t	 wname[MAX_PATH];
	char	 name[MAX_PATH * 3];
	size_t	 len;

	if (osdep_process_chain(osdep_pane_pid(fd), chain, nitems(chain),
	    wname) == 0 || wname[0] == L'\0')
		return (NULL);
	if (WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof name,
	    NULL, NULL) == 0)
		return (NULL);

	len = strlen(name);
	if (len > 4 && _stricmp(name + len - 4, ".exe") == 0)
		name[len - 4] = '\0';
	return (xstrdup(name));
}

/* Process parameters layout as far as CurrentDirectory (x64). */
struct osdep_process_parameters {
	BYTE		reserved[0x38];
	UNICODE_STRING	current_directory;
};

typedef NTSTATUS (NTAPI *osdep_nqip_fn)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG,
    PULONG);

/* Read the working directory of a process from its PEB. */
static int
osdep_process_cwd(DWORD pid, char *out, size_t outlen)
{
	static osdep_nqip_fn		 nqip;
	HANDLE				 h;
	PROCESS_BASIC_INFORMATION	 pbi;
	PEB				 peb;
	struct osdep_process_parameters	 params;
	wchar_t				 path[MAX_PATH * 2];
	USHORT				 bytes;
	size_t				 len;
	BOOL				 wow64;
	int				 ok = 0;

	if (nqip == NULL) {
		nqip = (osdep_nqip_fn)GetProcAddress(GetModuleHandleW(
		    L"ntdll.dll"), "NtQueryInformationProcess");
		if (nqip == NULL)
			return (0);
	}

	h = OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ, FALSE, pid);
	if (h == NULL)
		return (0);

	/* 32-bit processes have a different PEB layout; skip them. */
	if (IsWow64Process(h, &wow64) && wow64)
		goto out;

	if (nqip(h, ProcessBasicInformation, &pbi, sizeof pbi, NULL) != 0 ||
	    pbi.PebBaseAddress == NULL)
		goto out;
	if (!ReadProcessMemory(h, pbi.PebBaseAddress, &peb, sizeof peb, NULL))
		goto out;
	if (!ReadProcessMemory(h, peb.ProcessParameters, &params, sizeof params,
	    NULL))
		goto out;

	bytes = params.current_directory.Length;
	if (bytes == 0 || bytes >= sizeof path)
		goto out;
	if (!ReadProcessMemory(h, params.current_directory.Buffer, path, bytes,
	    NULL))
		goto out;
	len = bytes / sizeof path[0];
	path[len] = L'\0';

	/* Drop the trailing separator except for a drive root like C:\. */
	if (len > 3 && path[len - 1] == L'\\')
		path[--len] = L'\0';

	if (WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)outlen, NULL,
	    NULL) != 0)
		ok = 1;
out:
	CloseHandle(h);
	return (ok);
}

/*
 * Get the current working directory of the foreground process in a pane,
 * falling back to its parents if it cannot be read.
 */
char *
osdep_get_cwd(int fd)
{
	static char	 target[MAX_PATH * 6];
	DWORD		 chain[OSDEP_MAX_DEPTH];
	wchar_t		 wname[MAX_PATH];
	u_int		 n;

	n = osdep_process_chain(osdep_pane_pid(fd), chain, nitems(chain),
	    wname);
	while (n > 0) {
		if (osdep_process_cwd(chain[--n], target, sizeof target))
			return (target);
	}
	return (NULL);
}

/*
 * Initialize the event system.
 */
struct event_base *
osdep_event_init(void)
{
	return (event_init());
}

#endif /* _WIN32 */
