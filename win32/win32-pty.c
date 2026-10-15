/*
 * ConPTY (pseudo console) panes and popups for Windows.
 *
 * libevent can only poll sockets, so two threads copy between the pseudo
 * console's pipes and one end of a socket pair: tmux uses the other end as
 * the pane's file descriptor.
 *
 * A pseudo console stays open after its process exits, so when the process
 * exits the console is closed, which flushes its last frame; the output
 * thread then shuts down the socket and tmux reads end of file after all
 * the output, as with a Unix pty.
 *
 * ClosePseudoConsole can block until the console host exits, which needs
 * its output drained, so closing is finished on a separate thread while the
 * output thread keeps reading. The structure is freed only once every
 * thread that uses it has exited.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

struct win32_pty {
	CRITICAL_SECTION	 lock;		/* protects hpc */
	HPCON			 hpc;

	HANDLE			 in_write;	/* input to the console */
	HANDLE			 out_read;	/* output from the console */
	SOCKET			 sock;		/* tmux end of the socket pair */
	SOCKET			 peer;		/* bridge end */
	HANDLE			 out_thread;
	HANDLE			 in_thread;

	pid_t			 pid;
	HANDLE			 process;
	HANDLE			 job;
	HANDLE			 exit_wait;
	HANDLE			 exit_thread;

	int			 kill;		/* kill the tree when closing */
};

static void	win32_pty_destroy(struct win32_pty *);

/* Send all of a buffer. */
static int
win32_pty_send_all(SOCKET s, const char *buf, DWORD len)
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

/*
 * Console output to socket. This keeps reading after tmux has gone, until
 * the console host exits, so it can never block on a full pipe.
 */
static DWORD WINAPI
win32_pty_out_thread(LPVOID arg)
{
	struct win32_pty	*pty = arg;
	char			 buf[16384];
	DWORD			 n;
	int			 discard = 0;

	for (;;) {
		if (!ReadFile(pty->out_read, buf, sizeof buf, &n, NULL))
			break;
		if (n == 0 || discard)
			continue;
		if (win32_pty_send_all(pty->peer, buf, n) != 0)
			discard = 1;
	}

	/* Tell tmux there is no more output. */
	shutdown(pty->peer, SD_SEND);
	return (0);
}

/* Socket to console input. */
static DWORD WINAPI
win32_pty_in_thread(LPVOID arg)
{
	struct win32_pty	*pty = arg;
	char			 buf[16384];
	DWORD			 written;
	int			 n;

	for (;;) {
		n = recv(pty->peer, buf, sizeof buf, 0);
		if (n <= 0)
			break;
		if (!WriteFile(pty->in_write, buf, n, &written, NULL))
			break;
	}
	return (0);
}

/* Close the pseudo console, once. */
static void
win32_pty_hangup(struct win32_pty *pty)
{
	HPCON	hpc;

	EnterCriticalSection(&pty->lock);
	hpc = pty->hpc;
	pty->hpc = NULL;
	LeaveCriticalSection(&pty->lock);

	/*
	 * This sends CTRL_CLOSE_EVENT to the processes on the console, may
	 * write a final frame and may wait for the console host to exit.
	 */
	if (hpc != NULL)
		ClosePseudoConsole(hpc);
}

/* The process exited: close the console so its output ends. */
static VOID CALLBACK
win32_pty_exit_cb(PVOID arg, __unused BOOLEAN timed_out)
{
	win32_pty_hangup(arg);
}

/* Fallback if a thread pool wait cannot be registered. */
static DWORD WINAPI
win32_pty_exit_thread(LPVOID arg)
{
	struct win32_pty	*pty = arg;

	WaitForSingleObject(pty->process, INFINITE);
	win32_pty_hangup(pty);
	return (0);
}

/* Start a process on a new pseudo console. */
struct win32_pty *
win32_pty_spawn(struct win32_spawn *sp, int sx, int sy, pid_t *pid,
    char **cause)
{
	struct win32_pty		*pty;
	HANDLE				 in_read = NULL, out_write = NULL, job;
	STARTUPINFOEXW			 si;
	PROCESS_INFORMATION		 pi;
	LPPROC_THREAD_ATTRIBUTE_LIST	 attrs = NULL;
	SIZE_T				 size = 0;
	COORD				 coord;
	HRESULT				 hr;
	DWORD				 flags;
	int				 sv[2];
	char				*msg;

	if (win32_spawn_prepare(sp, cause) != 0)
		return (NULL);

	pty = xcalloc(1, sizeof *pty);
	InitializeCriticalSection(&pty->lock);
	pty->sock = pty->peer = INVALID_SOCKET;
	memset(&pi, 0, sizeof pi);

	if (!CreatePipe(&in_read, &pty->in_write, NULL, 0) ||
	    !CreatePipe(&pty->out_read, &out_write, NULL, 0))
		goto fail_error;

	coord.X = (SHORT)(sx < 1 ? 1 : (sx > SHRT_MAX ? SHRT_MAX : sx));
	coord.Y = (SHORT)(sy < 1 ? 1 : (sy > SHRT_MAX ? SHRT_MAX : sy));
	hr = CreatePseudoConsole(coord, in_read, out_write, 0, &pty->hpc);
	if (FAILED(hr)) {
		pty->hpc = NULL;
		xasprintf(cause, "CreatePseudoConsole failed (%#lx)", (u_long)hr);
		goto fail;
	}
	/* The console host has its own copies. */
	CloseHandle(in_read);
	in_read = NULL;
	CloseHandle(out_write);
	out_write = NULL;

	if (socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, sv) != 0) {
		xasprintf(cause, "socketpair failed (%d)", WSAGetLastError());
		goto fail;
	}
	pty->sock = (SOCKET)sv[0];
	pty->peer = (SOCKET)sv[1];

	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;

	/*
	 * Make the child take its standard handles from the pseudo console
	 * instead of whatever the server has; cmd.exe exits at once if they
	 * are not a console.
	 */
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
	si.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
	si.StartupInfo.hStdError = INVALID_HANDLE_VALUE;

	InitializeProcThreadAttributeList(NULL, 1, 0, &size);
	attrs = xmalloc(size);
	if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
		free(attrs);
		attrs = NULL;
		goto fail_error;
	}
	if (!UpdateProcThreadAttribute(attrs, 0,
	    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pty->hpc, sizeof pty->hpc,
	    NULL, NULL))
		goto fail_error;
	si.lpAttributeList = attrs;

	/* No handles are inherited: the console provides the standard ones. */
	flags = EXTENDED_STARTUPINFO_PRESENT|CREATE_SUSPENDED;
	if (sp->envblock != NULL)
		flags |= CREATE_UNICODE_ENVIRONMENT;
	log_debug("%s: %ls", __func__, sp->cmdline);
	if (!CreateProcessW(sp->app, sp->cmdline, NULL, NULL, FALSE, flags,
	    sp->envblock, sp->wcwd, &si.StartupInfo, &pi)) {
		memset(&pi, 0, sizeof pi);
		goto fail_error;
	}
	DeleteProcThreadAttributeList(attrs);
	free(attrs);
	attrs = NULL;
	pty->pid = (pid_t)pi.dwProcessId;

	/* Put it in a job so the whole tree can be killed with the pane. */
	pty->job = win32_job_create();
	if (pty->job != NULL && !AssignProcessToJobObject(pty->job,
	    pi.hProcess)) {
		log_debug("%s: AssignProcessToJobObject failed (%lu)",
		    __func__, GetLastError());
		CloseHandle(pty->job);
		pty->job = NULL;
	}
	if (!DuplicateHandle(GetCurrentProcess(), pi.hProcess,
	    GetCurrentProcess(), &pty->process, 0, FALSE,
	    DUPLICATE_SAME_ACCESS)) {
		pty->process = NULL;
		goto fail_process;
	}

	pty->out_thread = CreateThread(NULL, 0, win32_pty_out_thread, pty, 0,
	    NULL);
	if (pty->out_thread == NULL)
		goto fail_process;
	pty->in_thread = CreateThread(NULL, 0, win32_pty_in_thread, pty, 0,
	    NULL);
	if (pty->in_thread == NULL)
		goto fail_process;
	if (!RegisterWaitForSingleObject(&pty->exit_wait, pty->process,
	    win32_pty_exit_cb, pty, INFINITE,
	    WT_EXECUTEONLYONCE|WT_EXECUTELONGFUNCTION)) {
		pty->exit_wait = NULL;
		pty->exit_thread = CreateThread(NULL, 0, win32_pty_exit_thread,
		    pty, 0, NULL);
		if (pty->exit_thread == NULL)
			goto fail_process;
	}

	/* The registry gets its own handles to the process and job. */
	if (pty->job == NULL || !DuplicateHandle(GetCurrentProcess(), pty->job,
	    GetCurrentProcess(), &job, 0, FALSE, DUPLICATE_SAME_ACCESS))
		job = NULL;
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	win32_process_add(pty->pid, pi.hProcess, job);

	win32_spawn_free(sp);
	*pid = pty->pid;
	return (pty);

fail_process:
	msg = win32_error_string(GetLastError());
	xasprintf(cause, "%s", msg);
	free(msg);
	TerminateProcess(pi.hProcess, 1);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	win32_pty_destroy(pty);
	win32_spawn_free(sp);
	return (NULL);

fail_error:
	msg = win32_error_string(GetLastError());
	xasprintf(cause, "%s", msg);
	free(msg);
fail:
	if (attrs != NULL) {
		DeleteProcThreadAttributeList(attrs);
		free(attrs);
	}
	if (in_read != NULL)
		CloseHandle(in_read);
	if (out_write != NULL)
		CloseHandle(out_write);
	win32_pty_destroy(pty);
	win32_spawn_free(sp);
	return (NULL);
}

/* Resize the pseudo console. */
int
win32_pty_resize(struct win32_pty *pty, int sx, int sy)
{
	COORD	coord;
	HRESULT	hr = E_HANDLE;

	if (pty == NULL)
		return (-1);
	coord.X = (SHORT)(sx < 1 ? 1 : (sx > SHRT_MAX ? SHRT_MAX : sx));
	coord.Y = (SHORT)(sy < 1 ? 1 : (sy > SHRT_MAX ? SHRT_MAX : sy));

	EnterCriticalSection(&pty->lock);
	if (pty->hpc != NULL)
		hr = ResizePseudoConsole(pty->hpc, coord);
	LeaveCriticalSection(&pty->lock);
	return (SUCCEEDED(hr) ? 0 : -1);
}

/* Wait for a thread to exit, cancelling blocked I/O if it takes too long. */
static int
win32_pty_join(HANDLE thread)
{
	if (thread == NULL)
		return (0);
	if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0) {
		CancelSynchronousIo(thread);
		if (WaitForSingleObject(thread, 2000) != WAIT_OBJECT_0)
			return (-1);
	}
	CloseHandle(thread);
	return (0);
}

/* Tear down a pseudo console; may block, so not on the main thread. */
static void
win32_pty_destroy(struct win32_pty *pty)
{
	DWORD	code = WIN32_SIGNAL_EXIT_CODE(SIGHUP);

	/* Nothing more goes to or from tmux. */
	if (pty->sock != INVALID_SOCKET) {
		closesocket(pty->sock);
		pty->sock = INVALID_SOCKET;
	}

	/* With nobody reading, closing the console could block on output. */
	if (pty->out_thread == NULL && pty->out_read != NULL) {
		CloseHandle(pty->out_read);
		pty->out_read = NULL;
	}

	/* Ask everything on the console to exit (like SIGHUP). */
	win32_pty_hangup(pty);

	/* Then if the pane was killed, take down the whole process tree. */
	if (pty->kill && pty->process != NULL) {
		WaitForSingleObject(pty->process, 1000);
		if (pty->job == NULL || !TerminateJobObject(pty->job, code))
			TerminateProcess(pty->process, code);
	}

	/* Make sure the exit callback is not running. */
	if (pty->exit_wait != NULL)
		UnregisterWaitEx(pty->exit_wait, INVALID_HANDLE_VALUE);
	if (win32_pty_join(pty->exit_thread) != 0 ||
	    win32_pty_join(pty->out_thread) != 0 ||
	    win32_pty_join(pty->in_thread) != 0) {
		/* Leak rather than free memory a thread still uses. */
		return;
	}

	if (pty->peer != INVALID_SOCKET)
		closesocket(pty->peer);
	if (pty->in_write != NULL)
		CloseHandle(pty->in_write);
	if (pty->out_read != NULL)
		CloseHandle(pty->out_read);
	if (pty->process != NULL)
		CloseHandle(pty->process);
	if (pty->job != NULL)
		CloseHandle(pty->job);
	DeleteCriticalSection(&pty->lock);
	free(pty);
}

static DWORD WINAPI
win32_pty_close_thread(LPVOID arg)
{
	win32_pty_destroy(arg);
	return (0);
}

/*
 * Close a pseudo console and its socket (the pane's fd). If the process is
 * still running it is killed with its children. This returns at once; the
 * rest happens on another thread.
 */
void
win32_pty_close(struct win32_pty *pty)
{
	HANDLE	thread;

	if (pty == NULL)
		return;
	log_debug("%s: pid %ld", __func__, (long)pty->pid);

	/* Stops the bridge threads forwarding: tmux has finished with it. */
	closesocket(pty->sock);
	pty->sock = INVALID_SOCKET;

	pty->kill = (WaitForSingleObject(pty->process, 0) == WAIT_TIMEOUT);

	thread = CreateThread(NULL, 0, win32_pty_close_thread, pty, 0, NULL);
	if (thread == NULL)
		win32_pty_destroy(pty);
	else
		CloseHandle(thread);
}

/* Get the socket tmux uses for the pseudo console. */
int
win32_pty_get_fd(struct win32_pty *pty)
{
	if (pty == NULL)
		return (-1);
	return ((int)pty->sock);
}

/*
 * Has everything been read from a nonblocking socket, up to the end of
 * file? This is how tmux knows a pane's last output has arrived.
 */
int
win32_socket_drained(int fd)
{
	char	c;
	int	n;

	n = recv((SOCKET)fd, &c, 1, MSG_PEEK);
	if (n > 0)
		return (0);
	if (n == 0)
		return (1);
	return (WSAGetLastError() != WSAEWOULDBLOCK);
}
