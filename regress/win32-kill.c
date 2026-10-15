/*
 * Windows kill() replacement (win32-process.c): an ID that is not a live
 * child must never reach an unrelated process. Job IDs are odd and
 * OpenProcess ignores the low two bits of a process ID, so signalling a job
 * that has been reaped used to terminate the process whose ID is the job
 * ID with those bits cleared.
 */

#include <sys/types.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/* What win32-process.c needs from the rest of tmux. */
void
log_debug(__unused const char *fmt, ...)
{
}

__dead void
fatalx(const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}

void
win32_signal_notify(__unused int signo)
{
}

int
win32_spawn_prepare(__unused struct win32_spawn *sp, char **cause)
{
	*cause = xstrdup("not supported");
	return (-1);
}

void
win32_spawn_free(__unused struct win32_spawn *sp)
{
}

char *
win32_error_string(DWORD error)
{
	char	*s;

	xasprintf(&s, "error %lu", (u_long)error);
	return (s);
}

char *
win32_from_wide(__unused const wchar_t *w)
{
	return (xstrdup(""));
}

HANDLE
win32_job_create(void)
{
	return (NULL);
}

int
win32_exit_status(DWORD code)
{
	return ((int)(code & 0xff) << 8);
}

/* Start a process that waits for a while. */
static HANDLE
start(DWORD *pid)
{
	STARTUPINFOW		 si;
	PROCESS_INFORMATION	 pi;
	wchar_t			 sysdir[MAX_PATH], app[MAX_PATH + 16];
	wchar_t			 cmdline[] = L"ping.exe -n 30 127.0.0.1";

	if (GetSystemDirectoryW(sysdir, MAX_PATH) == 0)
		fatalx("GetSystemDirectory failed");
	_snwprintf_s(app, MAX_PATH + 16, _TRUNCATE, L"%ls\\PING.EXE", sysdir);

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	if (!CreateProcessW(app, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW,
	    NULL, NULL, &si, &pi))
		fatalx("CreateProcess failed: %lu", GetLastError());
	CloseHandle(pi.hThread);
	*pid = pi.dwProcessId;
	return (pi.hProcess);
}

/* Is a process still running? */
static int
alive(HANDLE h, DWORD ms)
{
	return (WaitForSingleObject(h, ms) == WAIT_TIMEOUT);
}

int
main(void)
{
	HANDLE	victim, child, dup;
	DWORD	victim_pid, child_pid;
	pid_t	id, got;
	int	errors = 0, i, r, status;

	win32_process_init();

	/*
	 * A process that is not a child of tmux, with the IDs a job may have
	 * had: a terminating signal must fail with ESRCH and leave it alone.
	 */
	victim = start(&victim_pid);
	for (i = 1; i <= 3; i++) {
		id = (pid_t)(victim_pid | i);
		errno = 0;
		r = win32_process_kill(id, SIGTERM);
		if (r != -1 || errno != ESRCH) {
			fprintf(stderr, "kill(%ld, SIGTERM) = %d, errno %d; "
			    "expected -1, ESRCH\n", (long)id, r, errno);
			errors++;
		}
		if (!alive(victim, 200)) {
			fprintf(stderr, "kill(%ld) terminated process %lu\n",
			    (long)id, (u_long)victim_pid);
			errors++;
			break;
		}
	}

	/* Its real ID is still usable, as for a client's parent. */
	if (alive(victim, 0) && win32_process_kill(victim_pid, 0) != 0) {
		fprintf(stderr, "kill(%lu, 0) failed\n", (u_long)victim_pid);
		errors++;
	}

	/* A child is killed, reaped once, and then unknown. */
	child = start(&child_pid);
	if (!DuplicateHandle(GetCurrentProcess(), child, GetCurrentProcess(),
	    &dup, SYNCHRONIZE, FALSE, 0))
		fatalx("DuplicateHandle failed");
	win32_process_add(child_pid, child, NULL);
	if (win32_process_kill(child_pid, SIGTERM) != 0 || alive(dup, 5000)) {
		fprintf(stderr, "child %lu was not killed\n", (u_long)child_pid);
		errors++;
	}
	got = 0;
	for (i = 0; i < 50 && got == 0; i++) {
		got = win32_process_waitpid(child_pid, &status, WNOHANG);
		if (got == 0)
			Sleep(100);
	}
	if (got != (pid_t)child_pid) {
		fprintf(stderr, "waitpid(%lu) = %ld\n", (u_long)child_pid,
		    (long)got);
		errors++;
	}
	if (win32_process_waitpid(child_pid, &status, WNOHANG) != -1) {
		fprintf(stderr, "child %lu reaped twice\n", (u_long)child_pid);
		errors++;
	}
	CloseHandle(dup);

	if (!alive(victim, 0)) {
		fprintf(stderr, "process %lu is gone\n", (u_long)victim_pid);
		errors++;
	}
	TerminateProcess(victim, 0);
	CloseHandle(victim);

	if (errors != 0)
		return (1);
	printf("win32-kill: ok\n");
	return (0);
}
