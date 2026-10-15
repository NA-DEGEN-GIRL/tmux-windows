/*
 * Process management for Windows.
 *
 * Every child tmux creates is registered here with its process handle and
 * job object. A thread pool wait reports each exit through the signal pipe
 * (SIGCHLD) and waitpid() returns it; the handle is kept until then so the
 * PID cannot be reused, just as a Unix zombie keeps its PID. kill() uses
 * the stored job object so the child's whole process tree goes, like a
 * signal to a process group.
 *
 * Jobs (run-shell, if-shell, #(), copy-pipe, pipe-pane) get anonymous pipes
 * for their standard handles and a thread per direction copies between
 * those and one end of a socket pair, because libevent can only poll
 * sockets on Windows. CreateProcess is slow (tens to hundreds of
 * milliseconds), much slower than fork, and formats such as the status line
 * give up after 100 milliseconds, so jobs are started on a thread and are
 * known to tmux by an ID of their own until then.
 */

#include <sys/types.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

struct win32_child {
	pid_t			 pid;		/* PID, or ID for a job */
	DWORD			 process_id;
	HANDLE			 process;	/* NULL until started */
	HANDLE			 job;
	HANDLE			 wait;
	int			 kill;		/* signal sent before started */

	volatile LONG		 references;
	volatile LONG		 exited;
	int			 status;

	struct win32_child	*next;
};
static struct win32_child	*win32_children;
static CRITICAL_SECTION		 win32_children_lock;

/* Copies data between a child's pipes and a socket. */
struct win32_bridge {
	SOCKET			 peer;
	HANDLE			 out_read;	/* child stdout/stderr */
	HANDLE			 in_write;	/* child stdin */
	volatile LONG		 references;
};

/* A job being started. */
struct win32_start {
	struct win32_child	*ch;
	struct win32_bridge	*br;
	int			 stdio;

	wchar_t			*app;
	wchar_t			*cmdline;
	wchar_t			*envblock;
	wchar_t			*wcwd;
};

void
win32_process_init(void)
{
	InitializeCriticalSection(&win32_children_lock);
}

/* Drop a reference to a child, freeing it with the last. */
static void
win32_process_release(struct win32_child *ch)
{
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION	info;

	if (InterlockedDecrement(&ch->references) != 0)
		return;

	/* The callback has finished with ch, do not wait for it. */
	if (ch->wait != NULL)
		UnregisterWaitEx(ch->wait, NULL);

	if (ch->job != NULL) {
		/*
		 * The child exited: let anything it left running live on, as
		 * it would on Unix, rather than being killed with the job.
		 */
		memset(&info, 0, sizeof info);
		info.BasicLimitInformation.LimitFlags =
		    JOB_OBJECT_LIMIT_BREAKAWAY_OK;
		SetInformationJobObject(ch->job,
		    JobObjectExtendedLimitInformation, &info, sizeof info);
		CloseHandle(ch->job);
	}
	if (ch->process != NULL)
		CloseHandle(ch->process);
	free(ch);
}

/* Mark a child exited and tell the main loop. */
static void
win32_process_exited(struct win32_child *ch, int status)
{
	ch->status = status;
	InterlockedExchange(&ch->exited, 1);
	win32_signal_notify(SIGCHLD);
}

/* A child has exited. This runs on a thread pool thread. */
static VOID CALLBACK
win32_process_exit_cb(PVOID arg, __unused BOOLEAN timed_out)
{
	struct win32_child	*ch = arg;
	DWORD			 code;

	if (!GetExitCodeProcess(ch->process, &code))
		code = WIN32_SIGNAL_EXIT_CODE(SIGKILL);
	win32_process_exited(ch, win32_exit_status(code));
}

/* Fallback if a thread pool wait cannot be registered. */
static DWORD WINAPI
win32_process_wait_thread(LPVOID arg)
{
	struct win32_child	*ch = arg;

	WaitForSingleObject(ch->process, INFINITE);
	win32_process_exit_cb(ch, FALSE);
	win32_process_release(ch);
	return (0);
}

/* Start watching a child for exit. */
static int
win32_process_watch(struct win32_child *ch)
{
	HANDLE	wait, thread;

	if (RegisterWaitForSingleObject(&wait, ch->process,
	    win32_process_exit_cb, ch, INFINITE, WT_EXECUTEONLYONCE)) {
		ch->wait = wait;
		return (0);
	}

	InterlockedIncrement(&ch->references);
	thread = CreateThread(NULL, 0, win32_process_wait_thread, ch, 0, NULL);
	if (thread == NULL) {
		InterlockedDecrement(&ch->references);
		return (-1);
	}
	CloseHandle(thread);
	return (0);
}

/* Add a child to the list. */
static struct win32_child *
win32_process_insert(pid_t pid, int references)
{
	struct win32_child	*ch;

	ch = xcalloc(1, sizeof *ch);
	ch->pid = pid;
	ch->references = references;

	EnterCriticalSection(&win32_children_lock);
	ch->next = win32_children;
	win32_children = ch;
	LeaveCriticalSection(&win32_children_lock);
	return (ch);
}

/* Remove a child from the list. */
static void
win32_process_remove(struct win32_child *ch)
{
	struct win32_child	**chp;

	EnterCriticalSection(&win32_children_lock);
	for (chp = &win32_children; *chp != NULL; chp = &(*chp)->next) {
		if (*chp == ch) {
			*chp = ch->next;
			break;
		}
	}
	LeaveCriticalSection(&win32_children_lock);
}

/* Terminate a child's process tree. */
static void
win32_process_terminate(struct win32_child *ch, int sig)
{
	DWORD	code = WIN32_SIGNAL_EXIT_CODE(sig);

	if (ch->job == NULL || !TerminateJobObject(ch->job, code))
		TerminateProcess(ch->process, code);
}

/*
 * Register a started child (a pane): takes ownership of its process and
 * job handles.
 */
void
win32_process_add(pid_t pid, HANDLE process, HANDLE job)
{
	struct win32_child	*ch;

	ch = win32_process_insert(pid, 1);
	ch->process_id = (DWORD)pid;
	ch->process = process;
	ch->job = job;
	if (win32_process_watch(ch) != 0)
		fatalx("cannot watch process %ld", (long)pid);
	log_debug("%s: pid %ld", __func__, (long)pid);
}

/* waitpid() replacement: only WNOHANG is supported. */
pid_t
win32_process_waitpid(pid_t pid, int *status, __unused int options)
{
	struct win32_child	**chp, *ch = NULL;
	int			  found = 0;

	EnterCriticalSection(&win32_children_lock);
	for (chp = &win32_children; *chp != NULL; chp = &(*chp)->next) {
		if (pid != WAIT_ANY && (*chp)->pid != pid)
			continue;
		found = 1;
		if (InterlockedOr(&(*chp)->exited, 0)) {
			ch = *chp;
			*chp = ch->next;
			break;
		}
	}
	LeaveCriticalSection(&win32_children_lock);

	if (ch == NULL) {
		if (found)
			return (0);
		errno = ECHILD;
		return (-1);
	}

	pid = ch->pid;
	if (status != NULL)
		*status = ch->status;
	log_debug("%s: pid %ld (process %lu) status %#x", __func__, (long)pid,
	    (u_long)ch->process_id, ch->status);
	win32_process_release(ch);
	return (pid);
}

/* Does this signal terminate a process by default? */
static int
win32_process_kill_terminates(int sig)
{
	return (sig == SIGHUP || sig == SIGINT || sig == SIGQUIT ||
	    sig == SIGKILL || sig == SIGTERM);
}

/* kill() replacement. */
int
win32_process_kill(pid_t pid, int sig)
{
	struct win32_child	*ch;
	HANDLE			 h;
	DWORD			 code = WIN32_SIGNAL_EXIT_CODE(sig);

	if (pid == (pid_t)GetCurrentProcessId()) {
		/*
		 * Deliver through the signal pipe so the event loop can shut
		 * down cleanly; SIGKILL cannot be caught.
		 */
		if (sig == SIGKILL)
			TerminateProcess(GetCurrentProcess(), code);
		if (sig != 0)
			win32_signal_notify(sig);
		return (0);
	}

	EnterCriticalSection(&win32_children_lock);
	for (ch = win32_children; ch != NULL; ch = ch->next) {
		if (ch->pid == pid)
			break;
	}
	if (ch != NULL) {
		if (win32_process_kill_terminates(sig) &&
		    !InterlockedOr(&ch->exited, 0)) {
			log_debug("%s: pid %ld signal %d", __func__, (long)pid,
			    sig);
			if (ch->process == NULL)
				ch->kill = sig;	/* not started yet */
			else
				win32_process_terminate(ch, sig);
		}
		LeaveCriticalSection(&win32_children_lock);
		return (0);
	}
	LeaveCriticalSection(&win32_children_lock);

	/*
	 * Not a child that has not been reaped. Job IDs are not process IDs
	 * (and OpenProcess ignores the low two bits of a process ID, so one
	 * would open an unrelated process), so a job that is gone is gone.
	 */
	if (pid <= 0 || (pid & 3) != 0) {
		errno = ESRCH;
		return (-1);
	}

	/* Some other process, such as the parent of a client. */
	if (sig == 0) {
		h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (h == NULL) {
			errno = ESRCH;
			return (-1);
		}
		CloseHandle(h);
		return (0);
	}
	if (!win32_process_kill_terminates(sig))
		return (0);
	h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
	if (h == NULL) {
		errno = (GetLastError() == ERROR_INVALID_PARAMETER) ?
		    ESRCH : EPERM;
		return (-1);
	}
	TerminateProcess(h, code);
	CloseHandle(h);
	return (0);
}

/* Drop a reference to a bridge, freeing it with the last. */
static void
win32_bridge_release(struct win32_bridge *br)
{
	if (InterlockedDecrement(&br->references) != 0)
		return;
	closesocket(br->peer);
	free(br);
}

/* Send all of a buffer. */
static int
win32_send_all(SOCKET s, const char *buf, DWORD len)
{
	int	n;

	while (len > 0) {
		n = send(s, buf, len > INT_MAX ? INT_MAX : (int)len, 0);
		if (n <= 0)
			return (-1);
		buf += n;
		len -= n;
	}
	return (0);
}

/* Copy child output to the socket until the child closes it. */
static void
win32_bridge_out(struct win32_bridge *br)
{
	char	buf[16384];
	DWORD	n;

	for (;;) {
		if (!ReadFile(br->out_read, buf, sizeof buf, &n, NULL))
			break;	/* the child and its children closed it */
		if (n == 0)
			continue;
		if (win32_send_all(br->peer, buf, n) != 0)
			break;	/* tmux closed it, the child gets EPIPE */
	}
	CloseHandle(br->out_read);

	/* Tell tmux there is no more output. */
	shutdown(br->peer, SD_SEND);
	win32_bridge_release(br);
}

/* Socket to child input. */
static DWORD WINAPI
win32_bridge_in_thread(LPVOID arg)
{
	struct win32_bridge	*br = arg;
	char			 buf[16384];
	DWORD			 written;
	int			 n;

	for (;;) {
		n = recv(br->peer, buf, sizeof buf, 0);
		if (n <= 0)
			break;	/* tmux shut down writing or closed */
		if (!WriteFile(br->in_write, buf, n, &written, NULL))
			break;	/* the child is not reading any more */
	}

	/* The child sees end of file. */
	CloseHandle(br->in_write);
	win32_bridge_release(br);
	return (0);
}

/* Make a handle inheritable, for the child's end of a pipe. */
static int
win32_inheritable(HANDLE h)
{
	return (SetHandleInformation(h, HANDLE_FLAG_INHERIT,
	    HANDLE_FLAG_INHERIT) ? 0 : -1);
}

/* Add a handle to a list if not already there. */
static void
win32_handle_list_add(HANDLE *list, u_int *n, HANDLE h)
{
	u_int	i;

	for (i = 0; i < *n; i++) {
		if (list[i] == h)
			return;
	}
	list[(*n)++] = h;
}

/* Close a handle if it is open. */
static void
win32_close_handle(HANDLE *h)
{
	if (*h != NULL && *h != INVALID_HANDLE_VALUE)
		CloseHandle(*h);
	*h = NULL;
}

/* Free a start request. */
static void
win32_start_free(struct win32_start *st)
{
	free(st->app);
	free(st->cmdline);
	free(st->envblock);
	free(st->wcwd);
	free(st);
}

/*
 * Create a job's process, then become its output bridge. Nothing here may
 * use tmux's own state, which belongs to the main thread.
 */
static DWORD WINAPI
win32_start_thread(LPVOID arg)
{
	struct win32_start		*st = arg;
	struct win32_child		*ch = st->ch;
	struct win32_bridge		*br = st->br;
	SECURITY_ATTRIBUTES		 sa;
	STARTUPINFOEXW			 si;
	PROCESS_INFORMATION		 pi;
	LPPROC_THREAD_ATTRIBUTE_LIST	 attrs = NULL;
	SIZE_T				 size = 0;
	HANDLE				 in_read = NULL, in_write = NULL;
	HANDLE				 out_read = NULL, out_write = NULL;
	HANDLE				 null = NULL, job, thread, list[3];
	u_int				 nlist = 0;
	DWORD				 flags, error;
	int				 stdio = st->stdio, sig, status;
	char				*msg, *app, *line;

	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	sa.bInheritHandle = TRUE;
	null = CreateFileW(L"NUL", GENERIC_READ|GENERIC_WRITE,
	    FILE_SHARE_READ|FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
	if (null == INVALID_HANDLE_VALUE) {
		null = NULL;
		goto fail;
	}
	if (stdio & WIN32_STDIO_IN) {
		if (!CreatePipe(&in_read, &in_write, NULL, 65536) ||
		    win32_inheritable(in_read) != 0)
			goto fail;
	}
	if (stdio & (WIN32_STDIO_OUT|WIN32_STDIO_ERR)) {
		if (!CreatePipe(&out_read, &out_write, NULL, 65536) ||
		    win32_inheritable(out_write) != 0)
			goto fail;
	}

	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = (stdio & WIN32_STDIO_IN) ? in_read : null;
	si.StartupInfo.hStdOutput = (stdio & WIN32_STDIO_OUT) ? out_write : null;
	si.StartupInfo.hStdError = (stdio & WIN32_STDIO_ERR) ? out_write : null;
	win32_handle_list_add(list, &nlist, si.StartupInfo.hStdInput);
	win32_handle_list_add(list, &nlist, si.StartupInfo.hStdOutput);
	win32_handle_list_add(list, &nlist, si.StartupInfo.hStdError);

	/* Inherit only the standard handles, not every socket tmux has. */
	InitializeProcThreadAttributeList(NULL, 1, 0, &size);
	attrs = malloc(size);
	if (attrs == NULL)
		goto fail;
	if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
		free(attrs);
		attrs = NULL;
		goto fail;
	}
	if (!UpdateProcThreadAttribute(attrs, 0,
	    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, nlist * sizeof *list, NULL,
	    NULL))
		goto fail;
	si.lpAttributeList = attrs;

	/*
	 * CREATE_NO_WINDOW gives the child a console without a window, so
	 * console programs it starts do not open windows of their own.
	 */
	flags = CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT;
	if (st->envblock != NULL)
		flags |= CREATE_UNICODE_ENVIRONMENT;
	if (!CreateProcessW(st->app, st->cmdline, NULL, NULL, TRUE, flags,
	    st->envblock, st->wcwd, &si.StartupInfo, &pi))
		goto fail;
	DeleteProcThreadAttributeList(attrs);
	free(attrs);
	attrs = NULL;
	win32_close_handle(&in_read);
	win32_close_handle(&out_write);
	win32_close_handle(&null);

	job = win32_job_create();
	if (job != NULL && !AssignProcessToJobObject(job, pi.hProcess))
		win32_close_handle(&job);

	EnterCriticalSection(&win32_children_lock);
	ch->process_id = pi.dwProcessId;
	ch->process = pi.hProcess;
	ch->job = job;
	sig = ch->kill;
	if (sig != 0)
		win32_process_terminate(ch, sig);	/* killed already */
	LeaveCriticalSection(&win32_children_lock);
	if (win32_process_watch(ch) != 0) {
		/* Cannot tell when it exits, so do not let it run. */
		TerminateProcess(pi.hProcess, 1);
		WaitForSingleObject(pi.hProcess, INFINITE);
		win32_process_exited(ch, 1 << 8);
	}

	if (in_write != NULL) {
		br->in_write = in_write;
		InterlockedIncrement(&br->references);
		thread = CreateThread(NULL, 0, win32_bridge_in_thread, br, 0,
		    NULL);
		if (thread == NULL) {
			InterlockedDecrement(&br->references);
			br->in_write = NULL;
			CloseHandle(in_write);	/* no input then */
		} else
			CloseHandle(thread);
	}

	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	win32_process_release(ch);

	if (out_read != NULL) {
		br->out_read = out_read;
		win32_bridge_out(br);
	} else
		win32_bridge_release(br);
	win32_start_free(st);
	return (0);

fail:
	/* Report it like a shell would and as the job's output. */
	error = GetLastError();
	if (attrs != NULL) {
		DeleteProcThreadAttributeList(attrs);
		free(attrs);
	}
	win32_close_handle(&in_read);
	win32_close_handle(&in_write);
	win32_close_handle(&out_read);
	win32_close_handle(&out_write);
	win32_close_handle(&null);

	if (stdio & (WIN32_STDIO_OUT|WIN32_STDIO_ERR)) {
		msg = win32_error_string(error);
		app = win32_from_wide(st->app);
		xasprintf(&line, "%s: %s\n", app, msg);
		win32_send_all(br->peer, line, (DWORD)strlen(line));
		free(line);
		free(app);
		free(msg);
	}
	if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
		status = 127 << 8;
	else
		status = 126 << 8;
	win32_process_exited(ch, status);
	win32_process_release(ch);

	shutdown(br->peer, SD_SEND);
	win32_bridge_release(br);
	win32_start_free(st);
	return (0);
}

/*
 * Start a job with its standard handles connected to a socket as selected
 * by stdio (WIN32_STDIO_*), the rest to NUL. Returns the job's ID, which
 * kill() and waitpid() understand, and the tmux end of the socket in fd.
 */
pid_t
win32_process_spawn(struct win32_spawn *sp, int stdio, int *fd,
    char **cause)
{
	static pid_t		 next_id;
	struct win32_child	*ch;
	struct win32_start	*st;
	struct win32_bridge	*br;
	HANDLE			 thread;
	pid_t			 pid;
	int			 sv[2];
	char			*msg;

	if (win32_spawn_prepare(sp, cause) != 0)
		return (-1);
	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, sv) != 0) {
		msg = win32_error_string(WSAGetLastError());
		xasprintf(cause, "socketpair failed: %s", msg);
		free(msg);
		win32_spawn_free(sp);
		return (-1);
	}
	log_debug("%s: %ls", __func__, sp->cmdline);

	/*
	 * Windows process IDs are multiples of four, so an odd ID cannot be
	 * confused with one.
	 */
	next_id = (next_id + 1) & 0x1fffffff;
	pid = (next_id << 2)|1;

	/* One reference for the list and one for the starting thread. */
	ch = win32_process_insert(pid, 2);

	br = xcalloc(1, sizeof *br);
	br->peer = (SOCKET)sv[1];
	br->references = 1;

	st = xcalloc(1, sizeof *st);
	st->ch = ch;
	st->br = br;
	st->stdio = stdio;
	st->app = sp->app;
	st->cmdline = sp->cmdline;
	st->envblock = sp->envblock;
	st->wcwd = sp->wcwd;
	sp->app = sp->cmdline = sp->envblock = sp->wcwd = NULL;

	thread = CreateThread(NULL, 0, win32_start_thread, st, 0, NULL);
	if (thread == NULL) {
		msg = win32_error_string(GetLastError());
		xasprintf(cause, "CreateThread failed: %s", msg);
		free(msg);
		win32_process_remove(ch);
		free(ch);
		win32_start_free(st);
		free(br);
		closesocket((SOCKET)sv[0]);
		closesocket((SOCKET)sv[1]);
		return (-1);
	}
	CloseHandle(thread);

	*fd = sv[0];
	return (pid);
}

/* Ignore console interrupts while the exec'd child handles them. */
static BOOL WINAPI
win32_process_exec_ctrl(DWORD type)
{
	return (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT);
}

/*
 * Run a shell command in place of this process, for tmux -c and
 * detach-client -E, and return its exit code. The child shares the console
 * and standard handles and dies with us, as if exec'd.
 */
int
win32_process_exec(const char *shell, const char *cmd)
{
	struct win32_spawn		 sp;
	STARTUPINFOEXW			 si;
	PROCESS_INFORMATION		 pi;
	LPPROC_THREAD_ATTRIBUTE_LIST	 attrs = NULL;
	SIZE_T				 size = 0;
	HANDLE				 std[3], list[3], job;
	DWORD				 code, ids[3] = {
		STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE
	};
	u_int				 i, nlist = 0;
	char				*cause, *msg;

	memset(&sp, 0, sizeof sp);
	sp.shell = shell;
	sp.cmd = cmd;
	if (win32_spawn_prepare(&sp, &cause) != 0) {
		fprintf(stderr, "%s\n", cause);
		free(cause);
		return (1);
	}

	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;
	for (i = 0; i < nitems(ids); i++) {
		std[i] = GetStdHandle(ids[i]);
		if (std[i] == NULL || std[i] == INVALID_HANDLE_VALUE ||
		    !DuplicateHandle(GetCurrentProcess(), std[i],
		    GetCurrentProcess(), &std[i], 0, TRUE,
		    DUPLICATE_SAME_ACCESS))
			std[i] = NULL;
		else
			win32_handle_list_add(list, &nlist, std[i]);
	}
	if (nlist != 0) {
		si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		si.StartupInfo.hStdInput = std[0];
		si.StartupInfo.hStdOutput = std[1];
		si.StartupInfo.hStdError = std[2];
		InitializeProcThreadAttributeList(NULL, 1, 0, &size);
		attrs = xmalloc(size);
		if (InitializeProcThreadAttributeList(attrs, 1, 0, &size) &&
		    UpdateProcThreadAttribute(attrs, 0,
		    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list,
		    nlist * sizeof *list, NULL, NULL))
			si.lpAttributeList = attrs;
	}

	log_debug("%s: %ls", __func__, sp.cmdline);
	if (!CreateProcessW(sp.app, sp.cmdline, NULL, NULL,
	    si.lpAttributeList != NULL,
	    CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
	    &si.StartupInfo, &pi)) {
		msg = win32_error_string(GetLastError());
		fprintf(stderr, "%s: %s\n", shell, msg);
		free(msg);
		return (127);
	}
	if (attrs != NULL) {
		if (si.lpAttributeList != NULL)
			DeleteProcThreadAttributeList(attrs);
		free(attrs);
	}
	for (i = 0; i < nitems(std); i++) {
		if (std[i] != NULL)
			CloseHandle(std[i]);
	}
	win32_spawn_free(&sp);

	/* Make sure the child goes if we are killed. */
	job = win32_job_create();
	if (job != NULL)
		AssignProcessToJobObject(job, pi.hProcess);

	SetConsoleCtrlHandler(win32_process_exec_ctrl, TRUE);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	WaitForSingleObject(pi.hProcess, INFINITE);
	if (!GetExitCodeProcess(pi.hProcess, &code))
		code = 1;
	CloseHandle(pi.hProcess);
	return ((int)code);
}

/*
 * Launch a detached tmux server process.
 * Runs: tmux.exe -D -S "<label>"
 * The -D flag tells tmux to run as a foreground server (no fork).
 */
void
win32_launch_server(const char *label)
{
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	char exe[MAX_PATH];
	char cmdline[MAX_PATH * 2];

	if (GetModuleFileNameA(NULL, exe, sizeof exe) == 0)
		return;

	snprintf(cmdline, sizeof cmdline,
	    "\"%s\" -D -S \"%s\"", exe, label);

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	memset(&pi, 0, sizeof pi);

	/*
	 * CREATE_BREAKAWAY_FROM_JOB detaches the server from any job object
	 * the parent belongs to (e.g., sshd's session job). Without this,
	 * closing an SSH connection kills the tmux server along with all
	 * processes in the job. Fall back without the flag if the job
	 * doesn't allow breakaway (ERROR_ACCESS_DENIED).
	 */
	if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
	    DETACHED_PROCESS | CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB,
	    NULL, NULL, &si, &pi)) {
		if (GetLastError() != ERROR_ACCESS_DENIED)
			return;
		if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
		    DETACHED_PROCESS | CREATE_NO_WINDOW, NULL, NULL,
		    &si, &pi))
			return;
	}

	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
}
