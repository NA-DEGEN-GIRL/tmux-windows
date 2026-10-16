/*
 * POSIX compatibility shims for Windows.
 * Implements functions declared in win32-platform.h.
 */

/* For rand_s(). */
#define _CRT_RAND_S

#include <sys/types.h>
#include <sys/stat.h>

#include <crtdbg.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "win32-platform.h"

#include <corecrt_startup.h>

/*
 * MSVC CRT provides _environ; POSIX code expects environ.
 * We provide the symbol and an init function to wire it up.
 */
char **environ = NULL;

void
win32_init_environ(void)
{
	environ = _environ;
}

/* flock() using LockFileEx/UnlockFileEx. */
int
win32_flock(int fd, int operation)
{
	HANDLE h;
	OVERLAPPED ov;
	DWORD flags = 0;

	h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return (-1);
	}

	memset(&ov, 0, sizeof ov);

	if (operation & LOCK_UN)
		return UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov) ? 0 : -1;

	if (operation & LOCK_EX)
		flags |= LOCKFILE_EXCLUSIVE_LOCK;
	if (operation & LOCK_NB)
		flags |= LOCKFILE_FAIL_IMMEDIATELY;

	if (!LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov)) {
		if (GetLastError() == ERROR_LOCK_VIOLATION)
			errno = EAGAIN;
		else
			errno = EIO;
		return (-1);
	}
	return (0);
}

/* closefrom: close all fds >= lowfd. */
void
win32_closefrom(int lowfd)
{
	int maxfd, fd;

	maxfd = _getmaxstdio();
	if (maxfd < 256)
		maxfd = 256;
	for (fd = lowfd; fd < maxfd; fd++)
		_close(fd); /* ignore errors */
}

/* clock_gettime using QueryPerformanceCounter. */
int
win32_clock_gettime(int clk_id, struct timespec *tp)
{
	static LARGE_INTEGER freq;
	static int freq_init = 0;
	LARGE_INTEGER counter;
	FILETIME ft;
	ULARGE_INTEGER uli;

	if (clk_id == CLOCK_MONOTONIC) {
		if (!freq_init) {
			QueryPerformanceFrequency(&freq);
			freq_init = 1;
		}
		QueryPerformanceCounter(&counter);
		tp->tv_sec = (time_t)(counter.QuadPart / freq.QuadPart);
		tp->tv_nsec = (long)(((counter.QuadPart % freq.QuadPart) *
		    1000000000LL) / freq.QuadPart);
		return (0);
	}

	/* CLOCK_REALTIME */
	GetSystemTimeAsFileTime(&ft);
	uli.LowPart = ft.dwLowDateTime;
	uli.HighPart = ft.dwHighDateTime;
	/* Windows FILETIME epoch is 1601-01-01, Unix is 1970-01-01. */
	uli.QuadPart -= 116444736000000000ULL;
	tp->tv_sec = (time_t)(uli.QuadPart / 10000000ULL);
	tp->tv_nsec = (long)((uli.QuadPart % 10000000ULL) * 100);
	return (0);
}

/* gettimeofday. */
int
win32_gettimeofday(struct timeval *tv, struct timezone *tz)
{
	FILETIME ft;
	ULARGE_INTEGER uli;

	(void)tz;
	GetSystemTimeAsFileTime(&ft);
	uli.LowPart = ft.dwLowDateTime;
	uli.HighPart = ft.dwHighDateTime;
	uli.QuadPart -= 116444736000000000ULL;
	tv->tv_sec = (long)(uli.QuadPart / 10000000ULL);
	tv->tv_usec = (long)((uli.QuadPart % 10000000ULL) / 10);
	return (0);
}

/* getpwuid. */
static struct passwd pw_entry;
static char pw_name_buf[256];
static char pw_dir_buf[MAX_PATH];

struct passwd *
win32_getpwuid(uid_t uid)
{
	DWORD size;

	(void)uid;

	size = sizeof pw_name_buf;
	if (!GetUserNameA(pw_name_buf, &size))
		strncpy(pw_name_buf, "user", sizeof pw_name_buf);

	if (GetEnvironmentVariableA("USERPROFILE", pw_dir_buf,
	    sizeof pw_dir_buf) == 0)
		strncpy(pw_dir_buf, "C:\\", sizeof pw_dir_buf);

	pw_entry.pw_name = pw_name_buf;
	pw_entry.pw_dir = pw_dir_buf;
	pw_entry.pw_shell = _PATH_BSHELL;
	pw_entry.pw_uid = 0;
	pw_entry.pw_gid = 0;

	return (&pw_entry);
}

static struct group gr_entry;
static char gr_name_buf[256];

struct group *
win32_getgrgid(gid_t gid)
{
	(void)gid;

	strncpy(gr_name_buf, "users", sizeof gr_name_buf);
	gr_name_buf[sizeof gr_name_buf - 1] = '\0';
	gr_entry.gr_name = gr_name_buf;
	gr_entry.gr_gid = 0;
	return (&gr_entry);
}

struct group *
win32_getgrnam(const char *name)
{
	(void)name;
	return win32_getgrgid(0);
}

/* getprogname. */
static char progname_buf[MAX_PATH];
static int progname_init = 0;

const char *
win32_getprogname(void)
{
	char *p;

	if (!progname_init) {
		GetModuleFileNameA(NULL, progname_buf, sizeof progname_buf);
		/* Extract basename. */
		p = strrchr(progname_buf, '\\');
		if (p != NULL) {
			memmove(progname_buf, p + 1,
			    strlen(p + 1) + 1);
		}
		/* Remove .exe extension. */
		p = strrchr(progname_buf, '.');
		if (p != NULL && _stricmp(p, ".exe") == 0)
			*p = '\0';
		progname_init = 1;
	}
	return (progname_buf);
}

/* waitpid: dequeue from our exit tracking. */
struct win32_child_exit {
	pid_t pid;
	int   status;
	struct win32_child_exit *next;
};
static struct win32_child_exit *child_exit_list = NULL;
static CRITICAL_SECTION child_exit_lock;
static int child_exit_init_done = 0;

static void
child_exit_ensure_init(void)
{
	if (!child_exit_init_done) {
		InitializeCriticalSection(&child_exit_lock);
		child_exit_init_done = 1;
	}
}

void
win32_child_exited(pid_t pid, int status)
{
	struct win32_child_exit *e;

	child_exit_ensure_init();
	e = calloc(1, sizeof *e);
	if (e == NULL)
		return;
	e->pid = pid;
	e->status = status;

	EnterCriticalSection(&child_exit_lock);
	e->next = child_exit_list;
	child_exit_list = e;
	LeaveCriticalSection(&child_exit_lock);
}

pid_t
win32_waitpid(pid_t pid, int *status, int options)
{
	struct win32_child_exit **pp, *e;

	(void)options;
	child_exit_ensure_init();

	EnterCriticalSection(&child_exit_lock);
	for (pp = &child_exit_list; *pp != NULL; pp = &(*pp)->next) {
		e = *pp;
		if (pid == WAIT_ANY || e->pid == pid) {
			*pp = e->next;
			LeaveCriticalSection(&child_exit_lock);
			if (status != NULL)
				*status = e->status;
			pid = e->pid;
			free(e);
			return (pid);
		}
	}
	LeaveCriticalSection(&child_exit_lock);

	errno = ECHILD;
	return ((pid_t)-1);
}

/* kill: send TerminateProcess. */
int
win32_kill(pid_t pid, int sig)
{
	HANDLE h;

	if (sig == 0) {
		/* Check if process exists. */
		h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (h == NULL) {
			errno = ESRCH;
			return (-1);
		}
		CloseHandle(h);
		return (0);
	}

	if (sig == SIGTERM || sig == SIGKILL || sig == SIGHUP) {
		/*
		 * Self-signal: deliver through the signal pipe so the
		 * event loop can shut down gracefully (cleanup IPC files,
		 * flush clients, etc.).  TerminateProcess would kill us
		 * instantly with no cleanup.
		 */
		if (pid == (pid_t)GetCurrentProcessId() && sig != SIGKILL) {
			win32_signal_notify(sig);
			return (0);
		}
		h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
		if (h == NULL) {
			errno = EPERM;
			return (-1);
		}
		TerminateProcess(h, 1);
		CloseHandle(h);
		return (0);
	}

	/* Other signals: ignore. */
	return (0);
}

/*
 * socketpair: create a connected TCP pair over loopback. The sockets are
 * not inheritable, so child processes do not hold tmux's connections open.
 * Another local process could connect to the listener first, so check that
 * the accepted connection really is ours.
 */
int
win32_socketpair(int domain, int type, int protocol, int sv[2])
{
	SOCKET			listener, s1 = INVALID_SOCKET;
	SOCKET			s2 = INVALID_SOCKET;
	struct sockaddr_in	addr, local, peer;
	int			addrlen, tries, one = 1;
	DWORD			flags;

	(void)domain;
	(void)type;
	(void)protocol;

	win32_wsa_init();

	flags = WSA_FLAG_OVERLAPPED|WSA_FLAG_NO_HANDLE_INHERIT;
	listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
	    flags);
	if (listener == INVALID_SOCKET)
		return (-1);

	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0; /* ephemeral */

	addrlen = sizeof addr;
	if (bind(listener, (struct sockaddr *)&addr, sizeof addr) != 0)
		goto fail;
	if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) != 0)
		goto fail;
	if (listen(listener, SOMAXCONN) != 0)
		goto fail;

	s1 = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, flags);
	if (s1 == INVALID_SOCKET)
		goto fail;
	if (connect(s1, (struct sockaddr *)&addr, sizeof addr) != 0)
		goto fail;
	addrlen = sizeof local;
	if (getsockname(s1, (struct sockaddr *)&local, &addrlen) != 0)
		goto fail;

	for (tries = 0; tries < 16; tries++) {
		addrlen = sizeof peer;
		s2 = accept(listener, (struct sockaddr *)&peer, &addrlen);
		if (s2 == INVALID_SOCKET)
			goto fail;
		if (addrlen == sizeof peer &&
		    peer.sin_port == local.sin_port &&
		    peer.sin_addr.s_addr == local.sin_addr.s_addr)
			break;
		closesocket(s2);
		s2 = INVALID_SOCKET;
	}
	if (s2 == INVALID_SOCKET)
		goto fail;
	SetHandleInformation((HANDLE)s2, HANDLE_FLAG_INHERIT, 0);
	closesocket(listener);

	/* Small writes (keystrokes) should not wait for Nagle. */
	setsockopt(s1, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
	setsockopt(s2, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);

	sv[0] = (int)s1;
	sv[1] = (int)s2;
	return (0);

fail:
	if (s1 != INVALID_SOCKET)
		closesocket(s1);
	closesocket(listener);
	return (-1);
}

/* getpeereid: always return uid/gid 0 on Windows. */
int
win32_getpeereid(int fd, uid_t *uid, gid_t *gid)
{
	(void)fd;
	*uid = 0;
	*gid = 0;
	return (0);
}

/* sendmsg: flatten iovecs and send. FD passing is a no-op. */
ssize_t
win32_sendmsg(int fd, const struct msghdr *msg, int flags)
{
	ssize_t total = 0;
	int i;

	for (i = 0; i < msg->msg_iovlen; i++) {
		ssize_t n = send((SOCKET)fd, (const char *)msg->msg_iov[i].iov_base,
		    (int)msg->msg_iov[i].iov_len, flags);
		if (n < 0) {
			if (total > 0)
				return (total);
			errno = WSAGetLastError();
			return (-1);
		}
		total += n;
		if ((size_t)n < msg->msg_iov[i].iov_len)
			break;
	}
	return (total);
}

/* recvmsg: receive into first iovec. No FD passing. */
ssize_t
win32_recvmsg(int fd, struct msghdr *msg, int flags)
{
	ssize_t n;

	if (msg->msg_iovlen < 1)
		return (0);

	n = recv((SOCKET)fd, (char *)msg->msg_iov[0].iov_base,
	    (int)msg->msg_iov[0].iov_len, flags);
	if (n < 0)
		errno = WSAGetLastError();

	/* No control message data on Windows. */
	msg->msg_controllen = 0;
	return (n);
}

#ifndef HAVE_STRSEP
char *
strsep(char **stringp, const char *delim)
{
	char *s, *tok;
	const char *spanp;
	int c, sc;

	if ((s = *stringp) == NULL)
		return (NULL);
	for (tok = s;;) {
		c = *s++;
		spanp = delim;
		do {
			if ((sc = *spanp++) == c) {
				if (c == 0)
					s = NULL;
				else
					s[-1] = 0;
				*stringp = s;
				return (tok);
			}
		} while (sc != 0);
	}
}
#endif

/* getpwnam: look up user by name; just return current user info. */
struct passwd *
win32_getpwnam(const char *name)
{
	(void)name;
	return win32_getpwuid(0);
}

/*
 * mkstemp: create and open a unique temporary file. The file is opened in
 * binary mode (no CRLF translation) and is not inherited by children.
 */
int
win32_mkstemp(char *tmpl)
{
	static const char	 chars[] =
	    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	size_t			 len, n, i;
	char			*x;
	unsigned int		 r;
	int			 fd, tries;

	len = strlen(tmpl);
	for (n = 0; n < len && tmpl[len - 1 - n] == 'X'; n++)
		/* nothing */;
	if (n < 6) {
		errno = EINVAL;
		return (-1);
	}
	x = tmpl + len - n;

	for (tries = 0; tries < 100; tries++) {
		for (i = 0; i < n; i++) {
			if (rand_s(&r) != 0)
				r = GetTickCount() + (unsigned int)(tries * 31 + i);
			x[i] = chars[r % (sizeof chars - 1)];
		}
		fd = _open(tmpl, _O_CREAT|_O_EXCL|_O_RDWR|_O_BINARY|_O_NOINHERIT,
		    _S_IREAD|_S_IWRITE);
		if (fd != -1 || errno != EEXIST)
			return (fd);
	}
	errno = EEXIST;
	return (-1);
}

/*
 * fnmatch() from musl libc (src/regex/fnmatch.c), adapted to decode UTF-8
 * into full code points with win32-unicode.c and to treat each invalid
 * UTF-8 byte as a single character. musl is MIT licensed:
 *
 * Copyright (c) 2005-2020 Rich Felker, et al.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * An implementation of what I call the "Sea of Stars" algorithm for POSIX
 * fnmatch(). The basic idea is that we factor the pattern into a head
 * component (which we match first and can reject without ever measuring
 * the length of the string), an optional tail component (which only exists
 * if the pattern contains at least one star), and an optional "sea of
 * stars", a set of star-separated components between the head and tail.
 * After the head and tail matches have been removed from the input string,
 * the components in the "sea of stars" are matched sequentially by
 * searching for their first occurrence past the end of the previous match.
 *
 * - Rich Felker, April 2012
 */

#define FNM_END 0
#define FNM_BRACKET -3
#define FNM_QUESTION -4
#define FNM_STAR -5

static int
fnm_str_next(const char *str, size_t n, size_t *step)
{
	uint32_t	c;
	int		k;

	k = win32_utf8_decode_lax(str, n, &c);
	*step = k;
	return ((int)c);
}

static int
fnm_pat_next(const char *pat, size_t m, size_t *step, int flags)
{
	uint32_t	c;
	size_t		k;
	int		z, esc = 0;

	if (!m || !*pat) {
		*step = 0;
		return (FNM_END);
	}
	*step = 1;
	if (pat[0] == '\\' && m > 1 && pat[1] && !(flags & FNM_NOESCAPE)) {
		*step = 2;
		pat++;
		m--;
		esc = 1;
		goto escaped;
	}
	if (pat[0] == '[') {
		k = 1;
		if (k < m && (pat[k] == '^' || pat[k] == '!'))
			k++;
		if (k < m && pat[k] == ']')
			k++;
		for (; k < m && pat[k] && pat[k] != ']'; k++) {
			if (k + 1 < m && pat[k + 1] && pat[k] == '[' &&
			    (pat[k + 1] == ':' || pat[k + 1] == '.' ||
			    pat[k + 1] == '=')) {
				z = pat[k + 1];
				k += 2;
				if (k < m && pat[k])
					k++;
				while (k < m && pat[k] &&
				    (pat[k - 1] != z || pat[k] != ']'))
					k++;
				if (k == m || !pat[k])
					break;
			}
		}
		if (k == m || !pat[k]) {
			/* Unterminated bracket: a literal '['. */
			*step = 1;
			return ('[');
		}
		*step = k + 1;
		return (FNM_BRACKET);
	}
	if (pat[0] == '*')
		return (FNM_STAR);
	if (pat[0] == '?')
		return (FNM_QUESTION);
escaped:
	*step = win32_utf8_decode_lax(pat, m, &c) + esc;
	return ((int)c);
}

static int
fnm_casefold(int k)
{
	int	c = (int)win32_towupper(k);

	return (c == k ? (int)win32_towlower(k) : c);
}

static int
fnm_match_bracket(const char *p, int k, int kfold)
{
	const char	*p0;
	uint32_t	 wc, wc2;
	char		 buf[16];
	int		 l, z, type, inv = 0;

	p++;
	if (*p == '^' || *p == '!') {
		inv = 1;
		p++;
	}
	if (*p == ']') {
		if (k == ']')
			return (!inv);
		p++;
	} else if (*p == '-') {
		if (k == '-')
			return (!inv);
		p++;
	}
	wc = (unsigned char)p[-1];
	for (; *p != ']'; p++) {
		if (p[0] == '-' && p[1] != ']') {
			l = win32_utf8_decode_lax(++p, 4, &wc2);
			if (l <= 0)
				return (0);
			if (wc <= wc2 &&
			    ((unsigned)k - wc <= wc2 - wc ||
			    (unsigned)kfold - wc <= wc2 - wc))
				return (!inv);
			p += l - 1;
			continue;
		}
		if (p[0] == '[' && (p[1] == ':' || p[1] == '.' || p[1] == '=')) {
			p0 = p + 2;
			z = p[1];
			p += 3;
			while (p[-1] != z || p[0] != ']')
				p++;
			if (z == ':' && p - 1 - p0 < 16) {
				memcpy(buf, p0, p - 1 - p0);
				buf[p - 1 - p0] = '\0';
				type = win32_wctype(buf);
				if (win32_iswctype(k, type) ||
				    win32_iswctype(kfold, type))
					return (!inv);
			}
			continue;
		}
		l = win32_utf8_decode_lax(p, 4, &wc);
		if (l <= 0)
			return (0);
		p += l - 1;
		if (((int)wc == k || (int)wc == kfold) &&
		    (p[1] != '-' || p[2] == ']'))
			return (!inv);
	}
	return (inv);
}

static int
fnm_match_one(int c, const char *pat, size_t pinc, int k, int kfold)
{
	if (c == FNM_BRACKET)
		return (fnm_match_bracket(pat, k, kfold));
	if (c == FNM_QUESTION)
		return (1);
	return (k == c || kfold == c);
}

static int
fnmatch_internal(const char *pat, size_t m, const char *str, size_t n,
    int flags)
{
	const char	*p, *ptail, *endpat, *s, *stail, *endstr;
	size_t		 pinc, sinc, tailcnt = 0, nchars;
	int		 c, k, kfold;

	if ((flags & FNM_PERIOD) && *str == '.' && *pat != '.' && *pat != '\\')
		return (FNM_NOMATCH);
	for (;;) {
		c = fnm_pat_next(pat, m, &pinc, flags);
		if (c == FNM_STAR) {
			pat++;
			m--;
			break;
		}
		k = fnm_str_next(str, n, &sinc);
		if (k == 0)
			return ((c == FNM_END) ? 0 : FNM_NOMATCH);
		if (c == FNM_END)
			return (FNM_NOMATCH);
		str += sinc;
		n -= sinc;
		kfold = (flags & FNM_CASEFOLD) ? fnm_casefold(k) : k;
		if (!fnm_match_one(c, pat, pinc, k, kfold))
			return (FNM_NOMATCH);
		pat += pinc;
		m -= pinc;
	}

	/* Compute real pat length if it was initially unknown. */
	m = strnlen(pat, m);
	endpat = pat + m;

	/* Find the last * in pat and count chars needed after it. */
	for (p = ptail = pat; p < endpat; p += pinc) {
		switch (fnm_pat_next(p, endpat - p, &pinc, flags)) {
		case FNM_STAR:
			tailcnt = 0;
			ptail = p + 1;
			break;
		default:
			tailcnt++;
			break;
		}
	}

	/*
	 * Compute real str length if it was initially unknown, and find the
	 * final tailcnt characters of str. Count forward so that invalid
	 * bytes are counted exactly as fnm_str_next() steps over them.
	 */
	n = strnlen(str, n);
	endstr = str + n;
	nchars = 0;
	for (s = str; s < endstr; s += sinc) {
		fnm_str_next(s, endstr - s, &sinc);
		nchars++;
	}
	if (nchars < tailcnt)
		return (FNM_NOMATCH);
	for (s = str; nchars > tailcnt; nchars--) {
		fnm_str_next(s, endstr - s, &sinc);
		s += sinc;
	}
	stail = s;

	/* Check that the pat and str tails match. */
	p = ptail;
	for (;;) {
		c = fnm_pat_next(p, endpat - p, &pinc, flags);
		p += pinc;
		if ((k = fnm_str_next(s, endstr - s, &sinc)) == 0) {
			if (c != FNM_END)
				return (FNM_NOMATCH);
			break;
		}
		s += sinc;
		kfold = (flags & FNM_CASEFOLD) ? fnm_casefold(k) : k;
		if (!fnm_match_one(c, p - pinc, pinc, k, kfold))
			return (FNM_NOMATCH);
	}

	/* We're all done with the tails now, so throw them out. */
	endstr = stail;
	endpat = ptail;

	/* Match pattern components until there are none left. */
	while (pat < endpat) {
		p = pat;
		s = str;
		for (;;) {
			c = fnm_pat_next(p, endpat - p, &pinc, flags);
			p += pinc;
			/* Encountering * completes/commits a component. */
			if (c == FNM_STAR) {
				pat = p;
				str = s;
				break;
			}
			k = fnm_str_next(s, endstr - s, &sinc);
			if (k == 0)
				return (FNM_NOMATCH);
			kfold = (flags & FNM_CASEFOLD) ? fnm_casefold(k) : k;
			if (!fnm_match_one(c, p - pinc, pinc, k, kfold))
				break;
			s += sinc;
		}
		if (c == FNM_STAR)
			continue;
		/* If we failed, advance str by one character. */
		fnm_str_next(str, endstr - str, &sinc);
		str += sinc;
	}

	return (0);
}

int
win32_fnmatch(const char *pat, const char *str, int flags)
{
	const char	*s, *p;
	size_t		 inc;
	int		 c;

	if (flags & FNM_PATHNAME) {
		for (;;) {
			for (s = str; *s && *s != '/'; s++)
				/* nothing */;
			for (p = pat; (c = fnm_pat_next(p, (size_t)-1, &inc,
			    flags)) != FNM_END && c != '/'; p += inc)
				/* nothing */;
			if (c != *s && (!*s || !(flags & FNM_LEADING_DIR)))
				return (FNM_NOMATCH);
			if (fnmatch_internal(pat, p - pat, str, s - str, flags))
				return (FNM_NOMATCH);
			if (!c)
				return (0);
			str = s + 1;
			pat = p + inc;
		}
	} else if (flags & FNM_LEADING_DIR) {
		for (s = str; *s; s++) {
			if (*s != '/')
				continue;
			if (!fnmatch_internal(pat, (size_t)-1, str, s - str,
			    flags))
				return (0);
		}
	}
	return (fnmatch_internal(pat, (size_t)-1, str, (size_t)-1, flags));
}

/*
 * Time functions. The UCRT versions abort through the invalid parameter
 * handler when given a struct tm with any field out of range (which is what
 * localtime_s() leaves behind for a time it can't convert) and strftime()
 * aborts on any conversion it doesn't know, including the GNU and BSD ones
 * people use in tmux configurations (%k, %l, %P, %-d and so on). These
 * versions accept any input.
 */

static const char *const win32_day_names[] = {
	"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
	"Saturday"
};
static const char *const win32_month_names[] = {
	"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December"
};

static int
win32_is_leap(long long year)
{
	return ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0);
}

/* Break down seconds since the epoch, ignoring time zones. */
static int
win32_breakdown(long long t, struct tm *tm)
{
	static const int	cum[] = {
		0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
	};
	long long		days, secs, z, era, doe, yoe, y, doy, mp, d, m;

	days = t / 86400;
	secs = t % 86400;
	if (secs < 0) {
		secs += 86400;
		days--;
	}

	/* Civil date from days since the epoch (Howard Hinnant). */
	z = days + 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp < 10 ? mp + 3 : mp - 9;
	if (m <= 2)
		y++;
	if (y - 1900 > INT_MAX || y - 1900 < INT_MIN)
		return (-1);

	memset(tm, 0, sizeof *tm);
	tm->tm_sec = (int)(secs % 60);
	tm->tm_min = (int)(secs / 60 % 60);
	tm->tm_hour = (int)(secs / 3600);
	tm->tm_mday = (int)d;
	tm->tm_mon = (int)m - 1;
	tm->tm_year = (int)(y - 1900);
	tm->tm_wday = (int)(((days % 7) + 11) % 7);
	tm->tm_yday = cum[m - 1] + (int)d - 1 + (m > 2 && win32_is_leap(y));
	tm->tm_isdst = 0;
	return (0);
}

static void
win32_epoch_tm(struct tm *tm)
{
	win32_breakdown(0, tm);
}

struct tm *
win32_gmtime_r(const time_t *t, struct tm *tm)
{
	if (t == NULL || tm == NULL) {
		errno = EINVAL;
		return (NULL);
	}
	if (win32_breakdown(*t, tm) != 0) {
		win32_epoch_tm(tm);
		errno = EOVERFLOW;
		return (NULL);
	}
	return (tm);
}

struct tm *
win32_localtime_r(const time_t *t, struct tm *tm)
{
	long	tz = 0;

	if (t == NULL || tm == NULL) {
		errno = EINVAL;
		return (NULL);
	}
	if (localtime_s(tm, t) == 0)
		return (tm);

	/*
	 * The UCRT only handles times between 1970 and 3000. Outside that,
	 * use the standard time offset without daylight saving.
	 */
	_get_timezone(&tz);
	if ((tz < 0 && *t > LLONG_MAX + tz) ||
	    (tz > 0 && *t < LLONG_MIN + tz) ||
	    win32_breakdown((long long)*t - tz, tm) != 0) {
		win32_epoch_tm(tm);
		errno = EOVERFLOW;
		return (NULL);
	}
	return (tm);
}

char *
win32_ctime_r(const time_t *t, char *buf)
{
	struct tm	tm;

	if (buf == NULL)
		return (NULL);
	if (t == NULL || win32_localtime_r(t, &tm) == NULL)
		win32_epoch_tm(&tm);
	snprintf(buf, 26, "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n",
	    win32_day_names[tm.tm_wday], win32_month_names[tm.tm_mon],
	    tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_year + 1900);
	return (buf);
}

/* The real CRT strftime, used for the locale-dependent conversions. */
#undef strftime

struct win32_strftime_out {
	char	*s;
	size_t	 max;
	size_t	 len;
};

static void
win32_strftime_add(struct win32_strftime_out *o, const char *s, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++) {
		if (o->len < o->max)
			o->s[o->len] = s[i];
		o->len++;
	}
}

static void
win32_strftime_pad(struct win32_strftime_out *o, char c, long long n)
{
	for (; n > 0; n--)
		win32_strftime_add(o, &c, 1);
}

/* Change the case of the ASCII letters in a string. */
static void
win32_strftime_case(char *s, int upper)
{
	for (; *s != '\0'; s++) {
		if (upper && *s >= 'a' && *s <= 'z')
			*s -= 'a' - 'A';
		else if (!upper && *s >= 'A' && *s <= 'Z')
			*s += 'a' - 'A';
	}
}

/* Is every field in range for the CRT's locale-dependent conversions? */
static int
win32_strftime_valid(const struct tm *tm)
{
	return (tm->tm_sec >= 0 && tm->tm_sec <= 60 &&
	    tm->tm_min >= 0 && tm->tm_min <= 59 &&
	    tm->tm_hour >= 0 && tm->tm_hour <= 23 &&
	    tm->tm_mday >= 1 && tm->tm_mday <= 31 &&
	    tm->tm_mon >= 0 && tm->tm_mon <= 11 &&
	    tm->tm_year >= -1900 && tm->tm_year <= 8099 &&
	    tm->tm_wday >= 0 && tm->tm_wday <= 6 &&
	    tm->tm_yday >= 0 && tm->tm_yday <= 365);
}

/* Days since the start of the ISO 8601 week-based year (from glibc). */
static long long
win32_iso_week_days(long long yday, long long wday)
{
	/* Add enough to the first operand of % to make it nonnegative. */
	long long	big_enough_multiple_of_7 = (366 / 7 + 2) * 7;

	return (yday - (yday - wday + 4 + big_enough_multiple_of_7) % 7 + 3);
}

/*
 * Expand one conversion into buf. Numbers are returned in *number with
 * *digits and *pad set to the default width and padding, other conversions
 * as a string. Returns -1 for an unknown conversion.
 */
static int
win32_strftime_conv(char conv, const struct tm *tm, char *buf, size_t size,
    long long *number, int *digits, char *pad, int *swapcase)
{
	struct tm	 copy;
	const char	*expand = NULL, *cfmt = NULL;
	long long	 year, days, d, value;
	int		 isnumber = 1, ok = 1;
	size_t		 n;

	*digits = 2;
	*pad = '0';
	*swapcase = 0;
	year = (long long)tm->tm_year + 1900;
	switch (conv) {
	case 'C':
		value = (year >= 0 ? year : year - 99) / 100;
		break;
	case 'd':
		value = tm->tm_mday;
		break;
	case 'e':
		value = tm->tm_mday;
		*pad = ' ';
		break;
	case 'G':
	case 'g':
	case 'V':
		days = win32_iso_week_days(tm->tm_yday, tm->tm_wday);
		if (days < 0) {
			/* This ISO week belongs to the previous year. */
			year--;
			days = win32_iso_week_days(tm->tm_yday +
			    (365 + win32_is_leap(year)), tm->tm_wday);
		} else {
			d = win32_iso_week_days(tm->tm_yday -
			    (365 + win32_is_leap(year)), tm->tm_wday);
			if (d >= 0) {
				/* This ISO week belongs to the next year. */
				year++;
				days = d;
			}
		}
		if (conv == 'G') {
			value = year;
			*digits = 1;
		} else if (conv == 'g')
			value = (year % 100 + 100) % 100;
		else
			value = days / 7 + 1;
		break;
	case 'H':
		value = tm->tm_hour;
		break;
	case 'I':
	case 'l':
		value = (tm->tm_hour % 12 + 12) % 12;
		if (value == 0)
			value = 12;
		if (conv == 'l')
			*pad = ' ';
		break;
	case 'j':
		value = (long long)tm->tm_yday + 1;
		*digits = 3;
		break;
	case 'k':
		value = tm->tm_hour;
		*pad = ' ';
		break;
	case 'M':
		value = tm->tm_min;
		break;
	case 'm':
		value = (long long)tm->tm_mon + 1;
		break;
	case 'S':
		value = tm->tm_sec;
		break;
	case 's':
		copy = *tm;
		value = _mktime64(&copy);
		*digits = 1;
		break;
	case 'U':
		value = ((long long)tm->tm_yday - tm->tm_wday + 7) / 7;
		break;
	case 'u':
		value = ((long long)tm->tm_wday - 1 + 7) % 7 + 1;
		*digits = 1;
		break;
	case 'W':
		value = ((long long)tm->tm_yday -
		    ((long long)tm->tm_wday - 1 + 7) % 7 + 7) / 7;
		break;
	case 'w':
		value = tm->tm_wday;
		*digits = 1;
		break;
	case 'Y':
		value = year;
		*digits = 1;
		break;
	case 'y':
		value = (year % 100 + 100) % 100;
		break;
	default:
		isnumber = 0;
		break;
	}
	if (isnumber) {
		*number = value;
		return (1);
	}

	switch (conv) {
	case '%':
		expand = "%";
		break;
	case 'n':
		expand = "\n";
		break;
	case 't':
		expand = "\t";
		break;
	case 'D':
		cfmt = "%m/%d/%y";
		break;
	case 'F':
		cfmt = "%Y-%m-%d";
		break;
	case 'R':
		cfmt = "%H:%M";
		break;
	case 'T':
		cfmt = "%H:%M:%S";
		break;
	case '+':
		cfmt = "%a %b %e %H:%M:%S %Z %Y";
		break;
	case 'a':
	case 'A':
		ok = (tm->tm_wday >= 0 && tm->tm_wday <= 6);
		*swapcase = 1;
		break;
	case 'b':
	case 'B':
	case 'h':
		ok = (tm->tm_mon >= 0 && tm->tm_mon <= 11);
		*swapcase = 1;
		break;
	case 'p':
	case 'P':
		*swapcase = -1;
		break;
	case 'c':
		if (!win32_strftime_valid(tm))
			cfmt = "%a %b %e %H:%M:%S %Y";
		break;
	case 'x':
		if (!win32_strftime_valid(tm))
			cfmt = "%m/%d/%y";
		break;
	case 'X':
		if (!win32_strftime_valid(tm))
			cfmt = "%H:%M:%S";
		break;
	case 'r':
		if (!win32_strftime_valid(tm))
			cfmt = "%I:%M:%S %p";
		break;
	case 'Z':
		*swapcase = -1;
		break;
	case 'z':
		break;
	default:
		return (-1);
	}

	if (expand != NULL) {
		snprintf(buf, size, "%s", expand);
		return (0);
	}
	if (cfmt != NULL) {
		if (win32_strftime(buf, size, cfmt, tm) == 0)
			*buf = '\0';
		return (0);
	}
	if (!ok) {
		snprintf(buf, size, "?");
		return (0);
	}

	/*
	 * Use the CRT for the locale-dependent conversions, with a struct tm
	 * which has every field in range so it can't abort.
	 */
	memset(&copy, 0, sizeof copy);
	copy.tm_mday = 1;
	copy.tm_year = 70;
	copy.tm_wday = 4;
	if (win32_strftime_valid(tm))
		copy = *tm;
	else {
		if (tm->tm_wday >= 0 && tm->tm_wday <= 6)
			copy.tm_wday = tm->tm_wday;
		if (tm->tm_mon >= 0 && tm->tm_mon <= 11)
			copy.tm_mon = tm->tm_mon;
		copy.tm_hour = (tm->tm_hour > 11) ? 12 : 0;
	}
	copy.tm_isdst = (tm->tm_isdst > 0);
	switch (conv) {
	case 'a':
		n = strftime(buf, size, "%a", &copy);
		break;
	case 'A':
		n = strftime(buf, size, "%A", &copy);
		break;
	case 'b':
	case 'h':
		n = strftime(buf, size, "%b", &copy);
		break;
	case 'B':
		n = strftime(buf, size, "%B", &copy);
		break;
	case 'p':
	case 'P':
		n = strftime(buf, size, "%p", &copy);
		break;
	case 'c':
		n = strftime(buf, size, "%c", &copy);
		break;
	case 'x':
		n = strftime(buf, size, "%x", &copy);
		break;
	case 'X':
		n = strftime(buf, size, "%X", &copy);
		break;
	case 'r':
		n = strftime(buf, size, "%r", &copy);
		break;
	case 'Z':
		n = strftime(buf, size, "%Z", &copy);
		break;
	default:
		n = strftime(buf, size, "%z", &copy);
		break;
	}
	buf[n] = '\0';
	if (conv == 'P')
		win32_strftime_case(buf, 0);
	return (0);
}

/*
 * strftime() supporting the POSIX conversions plus the GNU and BSD
 * extensions (%k %l %P %s %+ and the - _ 0 ^ # flags and field widths).
 * Unknown conversions are copied literally like glibc does.
 */
size_t
win32_strftime(char *s, size_t max, const char *fmt, const struct tm *tm)
{
	struct win32_strftime_out	 o = { s, max, 0 };
	const char			*start;
	char				 buf[512], numbuf[32], flag, pad;
	long long			 number, width, len;
	int				 r, digits, upcase, swapcase, doswap;

	if (s == NULL || fmt == NULL || tm == NULL || max == 0)
		return (0);

	while (*fmt != '\0') {
		if (*fmt != '%') {
			win32_strftime_add(&o, fmt++, 1);
			continue;
		}
		start = fmt++;

		/* Flags. */
		flag = '\0';
		upcase = swapcase = 0;
		for (;; fmt++) {
			if (*fmt == '_' || *fmt == '-' || *fmt == '0')
				flag = *fmt;
			else if (*fmt == '^')
				upcase = 1;
			else if (*fmt == '#')
				swapcase = 1;
			else
				break;
		}

		/* Field width. */
		width = -1;
		if (*fmt >= '0' && *fmt <= '9') {
			width = 0;
			while (*fmt >= '0' && *fmt <= '9') {
				if (width < 4096)
					width = width * 10 + (*fmt - '0');
				fmt++;
			}
			if (width > 4096)
				width = 4096;
		}

		/* Alternative representations are not supported. */
		if (*fmt == 'E' || *fmt == 'O')
			fmt++;

		if (*fmt == '\0') {
			/* A % at the end is copied literally. */
			win32_strftime_add(&o, start, fmt - start);
			break;
		}

		r = win32_strftime_conv(*fmt++, tm, buf, sizeof buf, &number,
		    &digits, &pad, &doswap);
		if (r == -1) {
			win32_strftime_add(&o, start, fmt - start);
			continue;
		}

		if (r == 1) {
			snprintf(numbuf, sizeof numbuf, "%lld",
			    number < 0 ? -number : number);
			len = strlen(numbuf) + (number < 0);
			if (flag == '_')
				pad = ' ';
			else if (flag == '0')
				pad = '0';
			if (width < 0)
				width = digits;
			if (flag == '-')
				width = 0;
			if (pad == '0') {
				if (number < 0)
					win32_strftime_add(&o, "-", 1);
				win32_strftime_pad(&o, '0', width - len);
			} else {
				win32_strftime_pad(&o, ' ', width - len);
				if (number < 0)
					win32_strftime_add(&o, "-", 1);
			}
			win32_strftime_add(&o, numbuf, strlen(numbuf));
			continue;
		}

		if (upcase || (swapcase && doswap == 1))
			win32_strftime_case(buf, 1);
		else if (swapcase && doswap == -1)
			win32_strftime_case(buf, 0);
		len = strlen(buf);
		if (flag != '-' && width > len)
			win32_strftime_pad(&o, flag == '0' ? '0' : ' ', width - len);
		win32_strftime_add(&o, buf, len);
	}

	if (o.len >= max)
		return (0);
	s[o.len] = '\0';
	return (o.len);
}

/*
 * CRT error handling. The default invalid parameter handler terminates the
 * process, which for the server means every session is lost because one
 * CRT call got a bad argument. Log and continue instead: the function then
 * fails with EINVAL like it would on other platforms. Debug CRT reports are
 * logged as well.
 */
static void	(*win32_crt_log)(const char *, ...);

static void
win32_invalid_parameter(const wchar_t *expr, const wchar_t *func,
    const wchar_t *file, unsigned int line, uintptr_t reserved)
{
	(void)reserved;
	if (win32_crt_log == NULL)
		return;
	if (func != NULL && file != NULL && expr != NULL) {
		win32_crt_log("CRT invalid parameter: %ls (%ls:%u): %ls", func,
		    file, line, expr);
	} else
		win32_crt_log("CRT invalid parameter");
}

#ifdef _DEBUG
static int
win32_crt_report(int type, char *message, int *retval)
{
	(void)retval;
	if (win32_crt_log != NULL)
		win32_crt_log("CRT report %d: %s", type, message);
	return (FALSE);
}
#endif

void
win32_crt_init(void (*logfn)(const char *, ...))
{
	win32_crt_log = logfn;
	_set_invalid_parameter_handler(win32_invalid_parameter);
#ifdef _DEBUG
	_CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, win32_crt_report);
#endif
}

/*
 * Replace argv with UTF-8 strings made from the wide command line. The CRT
 * builds argv in the ANSI code page, which loses every character outside
 * it (such as emoji) and gives tmux non-UTF-8 bytes for the rest (such as
 * Korean in CP949), so send-keys, session names, paths and formats given
 * on the command line would be corrupted.
 *
 * Have the CRT split the wide command line itself so the arguments are
 * the same as the ones it gave main(): CommandLineToArgvW() has different
 * rules for doubled quotes ("") as used in cmd.exe and batch files.
 */
void
win32_utf8_args(int *argc, char ***argv)
{
	wchar_t	**wargv;
	char	**nargv;
	int	  wargc, i, n;

	if (_configure_wide_argv(_crt_argv_unexpanded_arguments) != 0)
		return;
	wargv = __wargv;
	wargc = __argc;
	if (wargv == NULL || wargc != *argc)
		return;
	nargv = calloc((size_t)wargc + 1, sizeof *nargv);
	if (nargv == NULL)
		return;
	for (i = 0; i < wargc; i++) {
		n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0,
		    NULL, NULL);
		if (n <= 0 || (nargv[i] = malloc(n)) == NULL)
			goto fail;
		if (WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nargv[i], n,
		    NULL, NULL) != n)
			goto fail;
	}
	*argc = wargc;
	*argv = nargv;
	return;

fail:
	for (i = 0; i < wargc; i++)
		free(nargv[i]);
	free(nargv);
}

/*
 * Set up the text support for the process: CRT error handling first, then
 * UTF-8 arguments.
 */
void
win32_text_init(int *argc, char ***argv, void (*logfn)(const char *, ...))
{
	win32_crt_init(logfn);
	win32_utf8_args(argc, argv);
}

/*
 * Terminfo parameterized string expansion, as in ncurses tparm(3): all of
 * %% %c %s %d %o %x %X with printf flags, width and precision, %p1-%p9,
 * %P and %g variables, %'c', %{nn}, %l, %+ %- %* %/ %m, %& %| %^, %= %> %<,
 * %A %O, %! %~, %i and %? %t %e %; conditionals.
 *
 * Parameters are numbers or strings; the caller says which with a mask
 * (tiparm_s) or, for tparm, they are worked out from the string. A string
 * parameter is only dereferenced if it is known to be a string. The result
 * is in a buffer that grows as needed and is valid until the next call.
 */
#define TPARM_STACK_SIZE 32
#define TPARM_NPARAMS 9

struct tparm_value {
	int		 isstring;
	intptr_t	 num;
	const char	*str;
};

struct tparm_state {
	struct tparm_value	 stack[TPARM_STACK_SIZE];
	int			 sp;
	char			*buf;
	size_t			 len;
	size_t			 size;
	int			 failed;
};

static char	*tparm_buf;
static size_t	 tparm_size;

static void
tparm_add(struct tparm_state *ts, const char *s, size_t n)
{
	size_t	 newsize;
	char	*newbuf;

	if (ts->failed)
		return;
	if (ts->len + n + 1 > ts->size) {
		newsize = ts->size == 0 ? 256 : ts->size;
		while (ts->len + n + 1 > newsize)
			newsize *= 2;
		newbuf = realloc(ts->buf, newsize);
		if (newbuf == NULL) {
			ts->failed = 1;
			return;
		}
		ts->buf = newbuf;
		ts->size = newsize;
	}
	memcpy(ts->buf + ts->len, s, n);
	ts->len += n;
	ts->buf[ts->len] = '\0';
}

static void
tparm_push_num(struct tparm_state *ts, intptr_t n)
{
	if (ts->sp < TPARM_STACK_SIZE) {
		ts->stack[ts->sp].isstring = 0;
		ts->stack[ts->sp].num = n;
		ts->stack[ts->sp].str = NULL;
		ts->sp++;
	}
}

static void
tparm_push(struct tparm_state *ts, const struct tparm_value *v)
{
	if (ts->sp < TPARM_STACK_SIZE)
		ts->stack[ts->sp++] = *v;
}

static intptr_t
tparm_pop_num(struct tparm_state *ts)
{
	if (ts->sp == 0)
		return (0);
	ts->sp--;
	if (ts->stack[ts->sp].isstring)
		return (0);
	return (ts->stack[ts->sp].num);
}

static const char *
tparm_pop_str(struct tparm_state *ts)
{
	if (ts->sp == 0)
		return ("");
	ts->sp--;
	if (!ts->stack[ts->sp].isstring || ts->stack[ts->sp].str == NULL)
		return ("");
	return (ts->stack[ts->sp].str);
}

/*
 * Skip a conditional part starting at p (just after %t or %e). Stops after
 * the matching %; or, if stop_at_else, the matching %e. Returns the position
 * to continue from.
 */
static const char *
tparm_skip(const char *p, int stop_at_else)
{
	int	depth = 0;

	while (*p != '\0') {
		if (*p++ != '%')
			continue;
		if (*p == '\0')
			break;
		switch (*p++) {
		case '?':
			depth++;
			break;
		case ';':
			if (depth == 0)
				return (p);
			depth--;
			break;
		case 'e':
			if (depth == 0 && stop_at_else)
				return (p);
			break;
		case '\'':
			if (*p != '\0')
				p++;
			if (*p == '\'')
				p++;
			break;
		}
	}
	return (p);
}

/*
 * Work out which parameters are used as strings: those pushed with %pN
 * immediately before %l or %s (with any printf flags, width and
 * precision). Returns a mask with bit N-1 set for each.
 */
static int
tparm_analyze(const char *p)
{
	const char	*q;
	int		 mask = 0, last = 0;

	while (*p != '\0') {
		if (*p++ != '%')
			continue;
		if (*p == '\0')
			break;
		if (*p == 'p' && p[1] >= '1' && p[1] <= '9') {
			last = p[1] - '0';
			p += 2;
			continue;
		}
		if (*p == '\'') {
			last = 0;
			if (*++p != '\0' && *++p == '\'')
				p++;
			continue;
		}
		q = p;
		if (*q == ':') {
			q++;
			while (*q == '-' || *q == '+' || *q == '#' || *q == ' ')
				q++;
		} else {
			while (*q == '#' || *q == ' ')
				q++;
		}
		while (*q >= '0' && *q <= '9')
			q++;
		if (*q == '.') {
			q++;
			while (*q >= '0' && *q <= '9')
				q++;
		}
		if (last != 0 && (*p == 'l' || *q == 's'))
			mask |= 1 << (last - 1);
		last = 0;
		p++;
	}
	return (mask);
}

/* Output a value with a printf-style format (%[[:]flags][width[.prec]]c). */
static const char *
tparm_printf(struct tparm_state *ts, const char *p)
{
	char		 fmt[32], out[64], *f = fmt, conv;
	const char	*str;
	char		*big;
	int		 width = 0, prec = -1, n;

	*f++ = '%';
	if (*p == ':')
		p++;
	while (*p == '-' || *p == '+' || *p == '#' || *p == ' ' || *p == '0') {
		if (f < fmt + 8)
			*f++ = *p;
		p++;
	}
	while (*p >= '0' && *p <= '9') {
		if (width < 1000)
			width = width * 10 + (*p - '0');
		p++;
	}
	if (*p == '.') {
		prec = 0;
		p++;
		while (*p >= '0' && *p <= '9') {
			if (prec < 1000)
				prec = prec * 10 + (*p - '0');
			p++;
		}
	}
	conv = *p;
	if (conv != 'd' && conv != 'o' && conv != 'x' && conv != 'X' &&
	    conv != 's') {
		/* Not a valid conversion: ignore it. */
		return (*p == '\0' ? p : p + 1);
	}
	p++;
	if (width != 0)
		f += snprintf(f, sizeof fmt - (f - fmt), "%d", width);
	if (prec >= 0)
		f += snprintf(f, sizeof fmt - (f - fmt), ".%d", prec);
	if (conv == 's') {
		*f++ = 's';
		*f = '\0';
		str = tparm_pop_str(ts);
		n = snprintf(NULL, 0, fmt, str);
		if (n > 0) {
			big = malloc(n + 1);
			if (big == NULL) {
				ts->failed = 1;
				return (p);
			}
			snprintf(big, n + 1, fmt, str);
			tparm_add(ts, big, n);
			free(big);
		}
		return (p);
	}
	*f++ = conv;
	*f = '\0';
	n = snprintf(out, sizeof out, fmt, (int)tparm_pop_num(ts));
	if (n > 0)
		tparm_add(ts, out, (size_t)n < sizeof out ? n : sizeof out - 1);
	return (p);
}

static char *
tparm_expand(const char *str, const intptr_t *args, int strmask)
{
	struct tparm_state	 ts;
	struct tparm_value	 params[TPARM_NPARAMS];
	intptr_t		 vars[26];
	static intptr_t		 static_vars[26];
	const char		*p = str, *s;
	intptr_t		 a, b;
	char			 c;
	int			 i, n, neg;

	if (str == NULL)
		return (NULL);

	memset(&ts, 0, sizeof ts);
	ts.buf = tparm_buf;
	ts.size = tparm_size;
	if (ts.size > 65536) {
		/* Don't keep a huge buffer from a previous large result. */
		free(ts.buf);
		ts.buf = NULL;
		ts.size = 0;
	}
	tparm_add(&ts, "", 0);

	for (i = 0; i < TPARM_NPARAMS; i++) {
		params[i].isstring = (strmask >> i) & 1;
		params[i].num = params[i].isstring ? 0 : args[i];
		params[i].str = params[i].isstring ?
		    (const char *)args[i] : NULL;
	}
	memset(vars, 0, sizeof vars);

	while (*p != '\0' && !ts.failed) {
		if (*p != '%') {
			tparm_add(&ts, p++, 1);
			continue;
		}
		p++;
		switch (c = *p) {
		case '\0':
			break;
		case '%':
			tparm_add(&ts, "%", 1);
			p++;
			break;
		case 'c':
			c = (char)tparm_pop_num(&ts);
			tparm_add(&ts, &c, 1);
			p++;
			break;
		case 's':
		case 'd':
		case 'o':
		case 'x':
		case 'X':
		case ':':
		case '.':
		case '#':
		case ' ':
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			p = tparm_printf(&ts, p);
			break;
		case 'p':
			p++;
			if (*p >= '1' && *p <= '9') {
				tparm_push(&ts, &params[*p - '1']);
				p++;
			}
			break;
		case 'P':
			p++;
			if (*p >= 'a' && *p <= 'z') {
				vars[*p - 'a'] = tparm_pop_num(&ts);
				p++;
			} else if (*p >= 'A' && *p <= 'Z') {
				static_vars[*p - 'A'] = tparm_pop_num(&ts);
				p++;
			}
			break;
		case 'g':
			p++;
			if (*p >= 'a' && *p <= 'z') {
				tparm_push_num(&ts, vars[*p - 'a']);
				p++;
			} else if (*p >= 'A' && *p <= 'Z') {
				tparm_push_num(&ts, static_vars[*p - 'A']);
				p++;
			}
			break;
		case '\'':
			p++;
			if (*p != '\0') {
				tparm_push_num(&ts, (unsigned char)*p);
				p++;
				if (*p == '\'')
					p++;
			}
			break;
		case '{':
			p++;
			neg = 0;
			if (*p == '-') {
				neg = 1;
				p++;
			}
			n = 0;
			while (*p >= '0' && *p <= '9') {
				if (n < 100000000)
					n = n * 10 + (*p - '0');
				p++;
			}
			if (*p == '}')
				p++;
			tparm_push_num(&ts, neg ? -n : n);
			break;
		case 'l':
			s = tparm_pop_str(&ts);
			tparm_push_num(&ts, (intptr_t)strlen(s));
			p++;
			break;
		case '+': case '-': case '*': case '/': case 'm':
		case '&': case '|': case '^':
		case '=': case '>': case '<': case 'A': case 'O':
			b = tparm_pop_num(&ts);
			a = tparm_pop_num(&ts);
			switch (c) {
			case '+':
				a = (intptr_t)((uintptr_t)a + (uintptr_t)b);
				break;
			case '-':
				a = (intptr_t)((uintptr_t)a - (uintptr_t)b);
				break;
			case '*':
				a = (intptr_t)((uintptr_t)a * (uintptr_t)b);
				break;
			case '/':
				a = (b == 0 || (b == -1 && a == INTPTR_MIN)) ?
				    0 : a / b;
				break;
			case 'm':
				a = (b == 0 || b == -1) ? 0 : a % b;
				break;
			case '&':
				a &= b;
				break;
			case '|':
				a |= b;
				break;
			case '^':
				a ^= b;
				break;
			case '=':
				a = (a == b);
				break;
			case '>':
				a = (a > b);
				break;
			case '<':
				a = (a < b);
				break;
			case 'A':
				a = (a && b);
				break;
			case 'O':
				a = (a || b);
				break;
			}
			tparm_push_num(&ts, a);
			p++;
			break;
		case '!':
			tparm_push_num(&ts, !tparm_pop_num(&ts));
			p++;
			break;
		case '~':
			tparm_push_num(&ts, ~tparm_pop_num(&ts));
			p++;
			break;
		case 'i':
			if (!params[0].isstring)
				params[0].num++;
			if (!params[1].isstring)
				params[1].num++;
			p++;
			break;
		case '?':
		case ';':
			p++;
			break;
		case 't':
			p++;
			if (!tparm_pop_num(&ts))
				p = tparm_skip(p, 1);
			break;
		case 'e':
			p = tparm_skip(p + 1, 0);
			break;
		default:
			/* Unknown: ignore it. */
			p++;
			break;
		}
	}

	if (ts.failed) {
		free(ts.buf);
		tparm_buf = NULL;
		tparm_size = 0;
		return (NULL);
	}
	tparm_buf = ts.buf;
	tparm_size = ts.size;
	return (ts.buf);
}

char *
tparm(const char *str, intptr_t p1, intptr_t p2, intptr_t p3, intptr_t p4,
    intptr_t p5, intptr_t p6, intptr_t p7, intptr_t p8, intptr_t p9)
{
	intptr_t	args[TPARM_NPARAMS] = { p1, p2, p3, p4, p5, p6, p7, p8, p9 };

	if (str == NULL)
		return (NULL);
	return (tparm_expand(str, args, tparm_analyze(str)));
}

/*
 * tiparm_s(3) from ncurses 6.4: expected is the number of parameters and
 * mask has a bit set for each one which is a string; numbers are passed as
 * int. Returns NULL if the string uses a number parameter as a string, so
 * a bad terminal-overrides entry can't make it dereference a number.
 */
char *
tiparm_s(int expected, int mask, const char *str, ...)
{
	va_list		ap;
	intptr_t	args[TPARM_NPARAMS] = { 0 };
	int		i, used;

	if (str == NULL || expected < 0 || expected > TPARM_NPARAMS)
		return (NULL);
	used = tparm_analyze(str);
	if (used & ~mask)
		return (NULL);

	va_start(ap, str);
	for (i = 0; i < expected; i++) {
		if (mask & (1 << i))
			args[i] = (intptr_t)va_arg(ap, const char *);
		else
			args[i] = va_arg(ap, int);
	}
	va_end(ap);
	return (tparm_expand(str, args, mask & ((1 << expected) - 1)));
}
