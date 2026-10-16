/*
 * OS-dependent functions for Windows.
 * Implements osdep_get_name(), osdep_get_cwd(), osdep_event_init() and the
 * UTF-8 argv fallback used by main().
 */

#ifdef _WIN32

#include <sys/types.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

#include <windows.h>
#include <winternl.h>
#include <shellapi.h>
#include <corecrt_startup.h>

/*
 * How long a snapshot of the process table is reused. Every pane with
 * automatic-rename asks for its foreground process twice a second while it
 * has output, so share one snapshot between them.
 */
#define OSDEP_SNAPSHOT_MS 250

/* NtQueryInformationProcess class returning the console host process. */
#define OSDEP_CONSOLE_HOST_CLASS ((PROCESSINFOCLASS)49)

#define OSDEP_STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)

/* Console host of a process not looked up yet. */
#define OSDEP_HOST_UNKNOWN ((DWORD)-1)

/* Age (in milliseconds) after which a process keeps its console host. */
#define OSDEP_HOST_SETTLE_MS 5000

/*
 * A shell that was busy more than this long (in milliseconds) after starting
 * a command has put it in the background. Shells start waiting for a
 * foreground command within a few milliseconds of starting it.
 */
#define OSDEP_JOB_SETTLE_MS 200

/*
 * Context switches of a shell's main thread between two looks that mean it
 * did some work, such as reading a key at its prompt (bash takes seven or
 * more for a key), rather than handling SIGCHLD for a background job while
 * it waits for a foreground one (bash takes three).
 */
#define OSDEP_SHELL_SWITCHES 6

/* Thread state of a waiting thread (KTHREAD_STATE Waiting). */
#define OSDEP_THREAD_WAITING 5

/*
 * KUSER_SHARED_DATA, mapped at this address in every process: the tick count
 * thread wait times are measured in, and its multiplier to milliseconds (as
 * GetTickCount uses them).
 */
#define OSDEP_SHARED_TICK_MULTIPLIER (*(volatile ULONG *)0x7ffe0004)
#define OSDEP_SHARED_TICK_COUNT (*(volatile ULONG *)0x7ffe0320)

struct osdep_proc {
	DWORD		 pid;
	DWORD		 ppid;
	ULONGLONG	 created;
	const wchar_t	*name;		/* image name, not terminated */
	u_int		 namelen;	/* in characters */
	DWORD		 host;		/* console host pid or 0 */
	int		 parent;	/* index of live parent or -1 */
	const SYSTEM_PROCESS_INFORMATION *spi;

	/* Per-query state. */
	int		 member;	/* attached to the pane's console */
	int		 lparent;	/* logical parent if member */
	int		 eligible;	/* may be the foreground process */
	int		 background;	/* in a shell's background job */
};

static struct {
	BYTE			*buf;
	ULONG			 bufsize;
	struct osdep_proc	*procs;
	u_int			 nprocs;
	u_int			*order;		/* indexes by creation time */
	ULONGLONG		 when;
	ULONGLONG		 now;		/* system time */
	ULONG			 tick;		/* tick count */
	u_int			 generation;
	int			 valid;
} osdep_snap;

/* When a shell was last seen to do some work. */
struct osdep_shell {
	DWORD		 pid;
	ULONGLONG	 created;
	u_int		 generation;	/* of the snapshot last looked at */
	ULONG		 switches;	/* of the main thread then */
	ULONGLONG	 busy;		/* system time it last stopped work */
};
static struct osdep_shell	*osdep_shell_list;
static u_int			 osdep_nshells;

/*
 * Console hosts already looked up, kept between snapshots: finding one means
 * opening the process, and the same processes are asked about every time.
 */
struct osdep_host {
	DWORD		 pid;
	ULONGLONG	 created;
	DWORD		 host;
};
static struct osdep_host	*osdep_host_list;
static u_int			 osdep_nhosts;

/* Processes that hand the terminal to their children like a Unix shell. */
static const char *osdep_shells[] = {
	"ash", "bash", "cmd", "csh", "dash", "elvish", "fish", "ksh", "mksh",
	"nu", "powershell", "pwsh", "sh", "tcsh", "xonsh", "zsh"
};

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
osdep_cmp_pid(const void *a, const void *b)
{
	const struct osdep_proc	*pa = a, *pb = b;

	if (pa->pid < pb->pid)
		return (-1);
	return (pa->pid > pb->pid);
}

static int
osdep_find(DWORD pid)
{
	u_int	lo = 0, hi = osdep_snap.nprocs, mid;

	while (lo < hi) {
		mid = lo + (hi - lo) / 2;
		if (osdep_snap.procs[mid].pid == pid)
			return ((int)mid);
		if (osdep_snap.procs[mid].pid < pid)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (-1);
}

static int
osdep_cmp_order(const void *a, const void *b)
{
	const struct osdep_proc	*pa = &osdep_snap.procs[*(const u_int *)a];
	const struct osdep_proc	*pb = &osdep_snap.procs[*(const u_int *)b];

	if (pa->created < pb->created)
		return (-1);
	if (pa->created > pb->created)
		return (1);
	return (osdep_cmp_pid(pa, pb));
}

/*
 * Take a snapshot of the processes in this session: one system call that
 * returns every process with its parent, creation time and image name, so no
 * process has to be opened.
 */
static int
osdep_snapshot(void)
{
	SYSTEM_PROCESS_INFORMATION	*spi;
	struct osdep_proc		*p;
	struct osdep_shell		*sh;
	struct osdep_host		*oh;
	FILETIME			 ft;
	ULONGLONG			 now = GetTickCount64();
	ULONG				 need = 0, offset;
	NTSTATUS			 status;
	DWORD				 session;
	u_int				 n, i;
	int				 parent;

	if (osdep_snap.valid && now - osdep_snap.when < OSDEP_SNAPSHOT_MS)
		return (1);
	osdep_snap.valid = 0;

	if (osdep_snap.bufsize == 0)
		osdep_snap.bufsize = 512 * 1024;
	for (;;) {
		osdep_snap.buf = xrealloc(osdep_snap.buf, osdep_snap.bufsize);
		status = NtQuerySystemInformation(SystemProcessInformation,
		    osdep_snap.buf, osdep_snap.bufsize, &need);
		if (status != OSDEP_STATUS_INFO_LENGTH_MISMATCH)
			break;
		osdep_snap.bufsize = need + 64 * 1024;
		if (osdep_snap.bufsize < need)
			return (0);
	}
	if (!NT_SUCCESS(status)) {
		log_debug("%s: NtQuerySystemInformation failed (%#lx)",
		    __func__, (u_long)status);
		return (0);
	}

	GetSystemTimeAsFileTime(&ft);
	osdep_snap.now = ((ULONGLONG)ft.dwHighDateTime << 32) |
	    ft.dwLowDateTime;
	osdep_snap.tick = OSDEP_SHARED_TICK_COUNT;

	if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
		session = (DWORD)-1;

	n = 0;
	offset = 0;
	for (;;) {
		spi = (SYSTEM_PROCESS_INFORMATION *)(osdep_snap.buf + offset);
		if (session == (DWORD)-1 || spi->SessionId == session) {
			if (n % 256 == 0) {
				osdep_snap.procs = xreallocarray(
				    osdep_snap.procs, n + 256,
				    sizeof *osdep_snap.procs);
			}
			p = &osdep_snap.procs[n++];
			memset(p, 0, sizeof *p);
			p->pid = (DWORD)(ULONG_PTR)spi->UniqueProcessId;
			/* Reserved2 is InheritedFromUniqueProcessId. */
			p->ppid = (DWORD)(ULONG_PTR)spi->Reserved2;
			/* Reserved1 + 24 is CreateTime. */
			memcpy(&p->created, spi->Reserved1 + 24,
			    sizeof p->created);
			p->name = spi->ImageName.Buffer;
			p->namelen = spi->ImageName.Length / sizeof (wchar_t);
			p->host = OSDEP_HOST_UNKNOWN;
			p->spi = spi;
		}
		if (spi->NextEntryOffset == 0)
			break;
		offset += spi->NextEntryOffset;
	}
	osdep_snap.nprocs = n;
	if (n == 0)
		return (0);

	qsort(osdep_snap.procs, n, sizeof *osdep_snap.procs, osdep_cmp_pid);
	osdep_snap.order = xreallocarray(osdep_snap.order, n,
	    sizeof *osdep_snap.order);
	for (i = 0; i < n; i++) {
		p = &osdep_snap.procs[i];
		osdep_snap.order[i] = i;

		/*
		 * A parent created after the child is a reused PID; treat the
		 * child as an orphan, like one whose parent has exited.
		 */
		parent = osdep_find(p->ppid);
		if (parent == (int)i ||
		    (parent != -1 &&
		    osdep_snap.procs[parent].created > p->created))
			parent = -1;
		p->parent = parent;
	}
	qsort(osdep_snap.order, n, sizeof *osdep_snap.order, osdep_cmp_order);

	/* Forget the shells that have exited. */
	for (i = 0; i < osdep_nshells; /* nothing */) {
		sh = &osdep_shell_list[i];
		parent = osdep_find(sh->pid);
		if (parent != -1 &&
		    osdep_snap.procs[parent].created == sh->created) {
			i++;
			continue;
		}
		*sh = osdep_shell_list[--osdep_nshells];
	}

	/* Reuse the console hosts of processes still running. */
	for (i = 0; i < osdep_nhosts; /* nothing */) {
		oh = &osdep_host_list[i];
		parent = osdep_find(oh->pid);
		if (parent != -1 &&
		    osdep_snap.procs[parent].created == oh->created) {
			osdep_snap.procs[parent].host = oh->host;
			i++;
			continue;
		}
		*oh = osdep_host_list[--osdep_nhosts];
	}

	osdep_snap.generation++;
	osdep_snap.when = now;
	osdep_snap.valid = 1;
	return (1);
}

/* Compare a process image name, ignoring any .exe suffix. */
static int
osdep_name_is(struct osdep_proc *p, const char *name)
{
	size_t	len = strlen(name), i;

	if (p->namelen != len &&
	    (p->namelen != len + 4 ||
	    _wcsnicmp(p->name + len, L".exe", 4) != 0))
		return (0);
	for (i = 0; i < len; i++) {
		if (p->name[i] > 0x7f ||
		    tolower((u_char)p->name[i]) != (u_char)name[i])
			return (0);
	}
	return (1);
}

static int
osdep_is_shell(struct osdep_proc *p)
{
	u_int	i;

	for (i = 0; i < nitems(osdep_shells); i++) {
		if (osdep_name_is(p, osdep_shells[i]))
			return (1);
	}
	return (0);
}

static int
osdep_is_console_host(struct osdep_proc *p)
{
	return (osdep_name_is(p, "conhost") || osdep_name_is(p, "openconsole"));
}

/* Get the console host (conhost or OpenConsole) a process is attached to. */
static DWORD
osdep_console_host(struct osdep_proc *p)
{
	HANDLE		h;
	ULONG_PTR	value = 0;

	if (p->host != OSDEP_HOST_UNKNOWN)
		return (p->host);
	p->host = 0;

	h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p->pid);
	if (h != NULL) {
		if (NT_SUCCESS(NtQueryInformationProcess(h,
		    OSDEP_CONSOLE_HOST_CLASS, &value, sizeof value, NULL)))
			p->host = (DWORD)(value & ~(ULONG_PTR)3);
		CloseHandle(h);
	}

	/*
	 * A new process may not be attached to its final console yet (a pane
	 * process first reports another console host, and MSYS2 starts its
	 * processes suspended), so remember only the hosts of older ones.
	 */
	if (osdep_snap.now - p->created < OSDEP_HOST_SETTLE_MS * 10000ULL)
		return (p->host);

	osdep_host_list = xreallocarray(osdep_host_list, osdep_nhosts + 1,
	    sizeof *osdep_host_list);
	osdep_host_list[osdep_nhosts].pid = p->pid;
	osdep_host_list[osdep_nhosts].created = p->created;
	osdep_host_list[osdep_nhosts].host = p->host;
	osdep_nhosts++;
	return (p->host);
}

/*
 * Look at the main thread (the oldest thread) of a shell, which reads its
 * input and waits for its foreground commands. Returns the shell's state with
 * the last time it stopped doing some work, judged from when its main thread
 * last started to wait and how much it has run since the previous look.
 */
static struct osdep_shell *
osdep_shell_look(struct osdep_proc *p)
{
	const SYSTEM_PROCESS_INFORMATION	*spi = p->spi;
	const SYSTEM_THREAD_INFORMATION		*t, *main = NULL;
	struct osdep_shell			*sh = NULL;
	ULONGLONG				 idle, stopped;
	ULONG					 i;

	for (i = 0; i < osdep_nshells; i++) {
		if (osdep_shell_list[i].pid == p->pid &&
		    osdep_shell_list[i].created == p->created) {
			sh = &osdep_shell_list[i];
			if (sh->generation == osdep_snap.generation)
				return (sh);
			break;
		}
	}

	t = (const SYSTEM_THREAD_INFORMATION *)(spi + 1);
	for (i = 0; i < spi->NumberOfThreads; i++) {
		/* Reserved1[2] is CreateTime. */
		if (main == NULL ||
		    t[i].Reserved1[2].QuadPart < main->Reserved1[2].QuadPart)
			main = &t[i];
	}
	if (main == NULL)
		return (NULL);

	/*
	 * Reserved2 is WaitTime, the tick count when the thread started to
	 * wait, and Reserved3 ContextSwitches.
	 */
	if (main->ThreadState != OSDEP_THREAD_WAITING)
		stopped = osdep_snap.now;
	else {
		idle = ((ULONGLONG)(osdep_snap.tick - main->Reserved2) *
		    OSDEP_SHARED_TICK_MULTIPLIER) >> 24;
		stopped = osdep_snap.now - idle * 10000;
	}

	if (sh == NULL) {
		osdep_shell_list = xreallocarray(osdep_shell_list,
		    osdep_nshells + 1, sizeof *osdep_shell_list);
		sh = &osdep_shell_list[osdep_nshells++];
		sh->pid = p->pid;
		sh->created = p->created;
		sh->busy = stopped;
	} else if (main->Reserved3 - sh->switches >= OSDEP_SHELL_SWITCHES &&
	    stopped > sh->busy)
		sh->busy = stopped;
	sh->switches = main->Reserved3;
	sh->generation = osdep_snap.generation;
	return (sh);
}

/*
 * Has a shell put a command it started in the background? Windows has no
 * process groups to say, so watch the shell: while it waits for a foreground
 * command it does nothing, but at its prompt it reads every key. So a command
 * is in the background once the shell has done some work after starting it.
 * A command started with & is taken to be in the foreground until the shell
 * reads a key.
 */
static int
osdep_is_background(struct osdep_proc *shell, struct osdep_proc *p)
{
	struct osdep_shell	*sh;

	if ((sh = osdep_shell_look(shell)) == NULL)
		return (0);
	return (sh->busy > p->created + OSDEP_JOB_SETTLE_MS * 10000ULL);
}

/*
 * Approximate the foreground process of a pane, which Unix gets from the
 * terminal's foreground process group.
 *
 * The members are the pane process and every later process attached to the
 * same pseudoconsole. Parent links are not enough: MSYS2 and Cygwin programs
 * (Git Bash) fork and exec through intermediate processes that exit, leaving
 * the command with a parent PID that no longer exists. Such orphans are
 * adopted by the newest older member, which is where the dead intermediate
 * came from as far as can be told.
 *
 * Like a shell giving the terminal to a job, only shells pass the foreground
 * on: a process is eligible if all its ancestors are shells and it is not in
 * a background job, and the newest eligible member wins. So a pane running
 * vim shows vim even while it runs a filter, one running an agent shows the
 * agent rather than the tools it starts, and a shell at its prompt shows the
 * shell even with jobs running in the background. A background command that
 * is not a shell adopts no orphans: it forks nothing that would outlive it.
 *
 * Fills chain with the foreground process followed by its ancestors, the
 * pane process last, and returns its length.
 */
static u_int
osdep_foreground(DWORD root_pid, int *chain, u_int size)
{
	struct osdep_proc	*p, *root, *parent;
	DWORD			 host;
	u_int			 i, n;
	int			 r, idx, last, best;

	if (root_pid == 0 || size == 0 || !osdep_snapshot())
		return (0);
	if ((r = osdep_find(root_pid)) == -1)
		return (0);
	root = &osdep_snap.procs[r];
	host = osdep_console_host(root);

	for (i = 0; i < osdep_snap.nprocs; i++)
		osdep_snap.procs[i].member = 0;
	root->member = 1;
	root->lparent = -1;
	root->eligible = 1;
	root->background = 0;
	best = last = r;

	/*
	 * Look at the shells on each query, so their work is noticed close to
	 * when it happened.
	 */
	if (osdep_is_shell(root))
		osdep_shell_look(root);

	for (i = 0; i < osdep_snap.nprocs; i++) {
		idx = osdep_snap.order[i];
		p = &osdep_snap.procs[idx];
		if (p->created < root->created || idx == r)
			continue;
		if (p->pid == host || osdep_is_console_host(p))
			continue;

		if (p->parent != -1 && !osdep_snap.procs[p->parent].member) {
			/* A live parent outside the pane: not ours. */
			continue;
		}
		if (p->parent == -1 && host == 0) {
			/* Without the console, orphans cannot be placed. */
			continue;
		}
		if (host != 0 && osdep_console_host(p) != host)
			continue;

		p->member = 1;
		p->lparent = p->parent != -1 ? p->parent : last;
		parent = &osdep_snap.procs[p->lparent];
		p->background = parent->background;
		p->eligible = 0;
		if (parent->eligible && osdep_is_shell(parent)) {
			if (osdep_is_background(parent, p))
				p->background = 1;
			else
				p->eligible = 1;
		}
		if (p->eligible) {
			best = idx;
			if (osdep_is_shell(p))
				osdep_shell_look(p);
		}
		if (!p->background || osdep_is_shell(p))
			last = idx;
	}

	n = 0;
	for (idx = best; idx != -1 && n < size;
	    idx = osdep_snap.procs[idx].lparent)
		chain[n++] = idx;
	return (n);
}

/*
 * Get the name of the foreground process in a pane, without any directory or
 * .exe suffix so names match what Unix tmux reports (bash, vim, node, ...).
 */
char *
osdep_get_name(int fd, __unused char *tty)
{
	struct osdep_proc	*p;
	int			 chain[1];
	char			*name;
	int			 len;
	u_int			 namelen;

	if (osdep_foreground(osdep_pane_pid(fd), chain, nitems(chain)) == 0)
		return (NULL);
	p = &osdep_snap.procs[chain[0]];

	namelen = p->namelen;
	if (namelen > 4 && _wcsnicmp(p->name + namelen - 4, L".exe", 4) == 0)
		namelen -= 4;
	if (namelen == 0)
		return (NULL);

	len = WideCharToMultiByte(CP_UTF8, 0, p->name, namelen, NULL, 0, NULL,
	    NULL);
	if (len <= 0)
		return (NULL);
	name = xmalloc(len + 1);
	WideCharToMultiByte(CP_UTF8, 0, p->name, namelen, name, len, NULL,
	    NULL);
	name[len] = '\0';
	return (name);
}

/* Process parameters layout as far as CurrentDirectory. */
struct osdep_process_parameters {
	BYTE		reserved[0x38];
	UNICODE_STRING	current_directory;
};

#ifdef _WIN64
/* The same for a 32-bit (WOW64) process. */
struct osdep_peb32 {
	BYTE		reserved[0x10];
	ULONG		process_parameters;
};
struct osdep_process_parameters32 {
	BYTE		reserved[0x24];
	USHORT		length;
	USHORT		maximum_length;
	ULONG		buffer;
};
#endif

/* Read the working directory of a process from its PEB. */
static int
osdep_process_cwd(DWORD pid, char *out, size_t outlen)
{
	HANDLE					 h;
	PROCESS_BASIC_INFORMATION		 pbi;
	PEB					 peb;
	struct osdep_process_parameters		 params;
#ifdef _WIN64
	ULONG_PTR				 peb32_addr = 0;
	struct osdep_peb32			 peb32;
	struct osdep_process_parameters32	 params32;
#endif
	wchar_t					 path[MAX_PATH * 4];
	const void				*addr;
	wchar_t					*start;
	USHORT					 bytes;
	size_t					 len;
	int					 ok = 0;

	h = OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ, FALSE, pid);
	if (h == NULL)
		return (0);

#ifdef _WIN64
	if (NT_SUCCESS(NtQueryInformationProcess(h, ProcessWow64Information,
	    &peb32_addr, sizeof peb32_addr, NULL)) && peb32_addr != 0) {
		if (!ReadProcessMemory(h, (void *)peb32_addr, &peb32,
		    sizeof peb32, NULL))
			goto out;
		if (!ReadProcessMemory(h,
		    (void *)(ULONG_PTR)peb32.process_parameters, &params32,
		    sizeof params32, NULL))
			goto out;
		bytes = params32.length;
		addr = (void *)(ULONG_PTR)params32.buffer;
	} else
#endif
	{
		if (!NT_SUCCESS(NtQueryInformationProcess(h,
		    ProcessBasicInformation, &pbi, sizeof pbi, NULL)) ||
		    pbi.PebBaseAddress == NULL)
			goto out;
		if (!ReadProcessMemory(h, pbi.PebBaseAddress, &peb, sizeof peb,
		    NULL))
			goto out;
		if (!ReadProcessMemory(h, peb.ProcessParameters, &params,
		    sizeof params, NULL))
			goto out;
		bytes = params.current_directory.Length;
		addr = params.current_directory.Buffer;
	}

	if (bytes == 0 || bytes >= sizeof path)
		goto out;
	if (!ReadProcessMemory(h, addr, path, bytes, NULL))
		goto out;
	len = bytes / sizeof path[0];
	path[len] = L'\0';

	/* Drop a \\?\ prefix. */
	start = path;
	if (wcsncmp(start, L"\\\\?\\UNC\\", 8) == 0) {
		start += 6;
		start[0] = L'\\';
	} else if (wcsncmp(start, L"\\\\?\\", 4) == 0)
		start += 4;
	len = wcslen(start);

	/* Drop the trailing separator except for a drive root like C:\. */
	if (len > 3 && start[len - 1] == L'\\')
		start[--len] = L'\0';

	if (WideCharToMultiByte(CP_UTF8, 0, start, -1, out, (int)outlen, NULL,
	    NULL) != 0)
		ok = 1;
out:
	CloseHandle(h);
	return (ok);
}

/*
 * Get the current working directory of the foreground process in a pane,
 * falling back to its ancestors if it cannot be read.
 */
char *
osdep_get_cwd(int fd)
{
	static char	 target[MAX_PATH * 12];
	int		 chain[16];
	u_int		 n, i;

	n = osdep_foreground(osdep_pane_pid(fd), chain, nitems(chain));
	for (i = 0; i < n; i++) {
		if (osdep_process_cwd(osdep_snap.procs[chain[i]].pid, target,
		    sizeof target))
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

/* Convert a UTF-16 string to a newly allocated UTF-8 string. */
static char *
osdep_utf8(const wchar_t *ws)
{
	char	*s;
	int	 len;

	len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
	if (len <= 0)
		return (xstrdup(""));
	s = xmalloc(len);
	WideCharToMultiByte(CP_UTF8, 0, ws, -1, s, len, NULL, NULL);
	return (s);
}

/*
 * The application manifest makes UTF-8 the process code page on Windows 10
 * 1903 and later, so argv and the CRT environment are already UTF-8. Older
 * Windows ignores it and converts both to the ANSI code page, losing
 * anything it cannot represent; rebuild them from the UTF-16 originals.
 * Must be called after win32_init_environ().
 */
void
win32_utf8_main_args(int *argc, char ***argv)
{
	wchar_t		**wargv, *wenv, *w;
	char		**nargv, **nenv;
	int		  wargc, i, local = 0;
	u_int		  n;

	if (GetACP() == CP_UTF8)
		return;

	/*
	 * Have the CRT split the UTF-16 command line with the same rules it
	 * used for argv; CommandLineToArgvW treats some quotes differently.
	 */
	wargv = NULL;
	if (_configure_wide_argv(_crt_argv_unexpanded_arguments) == 0) {
		wargv = __wargv;
		wargc = __argc;
	}
	if (wargv == NULL) {
		wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
		local = 1;
	}
	if (wargv != NULL && wargc == *argc) {
		nargv = xreallocarray(NULL, wargc + 1, sizeof *nargv);
		for (i = 0; i < wargc; i++)
			nargv[i] = osdep_utf8(wargv[i]);
		nargv[wargc] = NULL;
		*argv = nargv;
	}
	if (local && wargv != NULL)
		LocalFree(wargv);

	wenv = GetEnvironmentStringsW();
	if (wenv != NULL) {
		n = 0;
		nenv = NULL;
		for (w = wenv; *w != L'\0'; w += wcslen(w) + 1) {
			/* Skip the hidden per-drive directories (=C:=C:\). */
			if (*w == L'=')
				continue;
			nenv = xreallocarray(nenv, n + 2, sizeof *nenv);
			nenv[n++] = osdep_utf8(w);
		}
		if (nenv != NULL) {
			nenv[n] = NULL;
			environ = nenv;
		}
		FreeEnvironmentStringsW(wenv);
	}
}

#endif /* _WIN32 */
