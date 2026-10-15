/*
 * Process creation helpers for Windows: command lines, environment blocks,
 * working directories and exit status mapping for CreateProcessW.
 *
 * There is no execve(2) on Windows: a child gets a single command line
 * string that it splits into arguments itself, and the rules for that
 * depend on the program: cmd.exe has its own, MSYS2 and Cygwin programs
 * (Git Bash) use the Cygwin runtime's rules and native programs use the
 * Microsoft C runtime's. This file turns tmux's shell command or argument
 * vector into the command line that the target program will parse back
 * into the same arguments.
 */

#include <sys/types.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "tmux.h"

/* Variable holding a job's command for win32_cmdline_utf8. */
#define WIN32_JOB_VARIABLE "TMUX_WIN32_JOB"

/* A growable wide string. */
struct wbuf {
	wchar_t	*s;
	size_t	 len;
	size_t	 size;
};

static void
wbuf_addn(struct wbuf *b, const wchar_t *s, size_t n)
{
	if (b->len + n + 1 > b->size) {
		b->size = (b->len + n + 1) * 2;
		b->s = xreallocarray(b->s, b->size, sizeof *b->s);
	}
	wmemcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = L'\0';
}

static void
wbuf_add(struct wbuf *b, const wchar_t *s)
{
	wbuf_addn(b, s, wcslen(s));
}

static void
wbuf_addc(struct wbuf *b, wchar_t c)
{
	wbuf_addn(b, &c, 1);
}

static void
wbuf_addcn(struct wbuf *b, wchar_t c, size_t n)
{
	while (n-- != 0)
		wbuf_addc(b, c);
}

/* Convert UTF-8 to an allocated wide string. */
wchar_t *
win32_to_wide(const char *s)
{
	wchar_t	*w;
	int	 n;

	if (s == NULL)
		return (NULL);
	n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (n <= 0)
		return (xcalloc(1, sizeof *w));
	w = xreallocarray(NULL, n, sizeof *w);
	MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	return (w);
}

/* Convert a wide string to allocated UTF-8. */
char *
win32_from_wide(const wchar_t *w)
{
	char	*s;
	int	 n;

	if (w == NULL)
		return (NULL);
	n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (n <= 0)
		return (xstrdup(""));
	s = xmalloc(n);
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	return (s);
}

/* Describe a Windows error code. */
char *
win32_error_string(DWORD error)
{
	wchar_t	*text = NULL;
	char	*s, *msg;
	size_t	 len;

	if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|
	    FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
	    error, 0, (LPWSTR)&text, 0, NULL) == 0 || text == NULL) {
		xasprintf(&msg, "error %lu", (u_long)error);
		return (msg);
	}
	s = win32_from_wide(text);
	LocalFree(text);
	len = strlen(s);
	while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' ||
	    s[len - 1] == ' ' || s[len - 1] == '.'))
		s[--len] = '\0';
	xasprintf(&msg, "%s (%lu)", s, (u_long)error);
	free(s);
	return (msg);
}

/*
 * Map a Windows exit code to a wait status. Exit codes do not fit in the
 * eight bits of WEXITSTATUS, so keep a failure a failure and report
 * crashes (NTSTATUS exception codes) and tmux's own kills as signals.
 */
int
win32_exit_status(DWORD code)
{
	int	sig;

	switch (code) {
	case 0xC0000005:	/* STATUS_ACCESS_VIOLATION */
	case 0xC0000006:	/* STATUS_IN_PAGE_ERROR */
	case 0xC000008C:	/* STATUS_ARRAY_BOUNDS_EXCEEDED */
	case 0xC00000FD:	/* STATUS_STACK_OVERFLOW */
		sig = 11;	/* SIGSEGV */
		break;
	case 0xC000001D:	/* STATUS_ILLEGAL_INSTRUCTION */
	case 0xC0000096:	/* STATUS_PRIVILEGED_INSTRUCTION */
		sig = 4;	/* SIGILL */
		break;
	case 0xC000008D:	/* STATUS_FLOAT_DENORMAL_OPERAND */
	case 0xC000008E:	/* STATUS_FLOAT_DIVIDE_BY_ZERO */
	case 0xC000008F:	/* STATUS_FLOAT_INEXACT_RESULT */
	case 0xC0000090:	/* STATUS_FLOAT_INVALID_OPERATION */
	case 0xC0000091:	/* STATUS_FLOAT_OVERFLOW */
	case 0xC0000092:	/* STATUS_FLOAT_STACK_CHECK */
	case 0xC0000093:	/* STATUS_FLOAT_UNDERFLOW */
	case 0xC0000094:	/* STATUS_INTEGER_DIVIDE_BY_ZERO */
	case 0xC0000095:	/* STATUS_INTEGER_OVERFLOW */
		sig = 8;	/* SIGFPE */
		break;
	case 0x80000002:	/* STATUS_DATATYPE_MISALIGNMENT */
		sig = 7;	/* SIGBUS */
		break;
	case 0x80000003:	/* STATUS_BREAKPOINT */
	case 0x80000004:	/* STATUS_SINGLE_STEP */
		sig = 5;	/* SIGTRAP */
		break;
	case 0xC000013A:	/* STATUS_CONTROL_C_EXIT */
		sig = 2;	/* SIGINT */
		break;
	case 0x40000015:	/* STATUS_FATAL_APP_EXIT, abort() */
	case 0xC0000374:	/* STATUS_HEAP_CORRUPTION */
	case 0xC0000409:	/* STATUS_STACK_BUFFER_OVERRUN, __fastfail */
		sig = 6;	/* SIGABRT */
		break;
	default:
		if ((code & 0xFFFFFF00UL) == WIN32_SIGNAL_EXIT_CODE(0)) {
			sig = code & 0x7f;
			if (sig == 0)
				sig = 9;	/* SIGKILL */
			break;
		}
		if (code <= 255)
			return ((int)code << 8);
		if (code >= 0xFFFFFF01UL) {
			/* Small negative codes, exit(-1) is 255 as on Unix. */
			return ((int)(code & 0xff) << 8);
		}
		if (code == 9009) {
			/* cmd.exe's "is not recognized" is 127 in sh. */
			return (127 << 8);
		}
		if ((code & 0xC0000000UL) == 0xC0000000UL) {
			/* Any other NTSTATUS error: abnormal termination. */
			sig = 6;	/* SIGABRT */
			break;
		}
		return (255 << 8);
	}
	return (sig);
}

/* Create a job object that kills its processes when the last handle closes. */
HANDLE
win32_job_create(void)
{
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION	info;
	HANDLE					job;

	job = CreateJobObjectW(NULL, NULL);
	if (job == NULL)
		return (NULL);

	/*
	 * Allow breakaway so a tmux started inside a pane can still create a
	 * detached server that survives the pane being killed.
	 */
	memset(&info, 0, sizeof info);
	info.BasicLimitInformation.LimitFlags =
	    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_BREAKAWAY_OK;
	if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
	    &info, sizeof info)) {
		CloseHandle(job);
		return (NULL);
	}
	return (job);
}

/*
 * Convert an MSYS or Cygwin style path (/c/Users/x, /cygdrive/c/x) or one
 * with forward slashes to a Windows path.
 */
char *
win32_path_from_posix(const char *path)
{
	char		*out, *cp;
	const char	*rest = NULL;
	char		 drive = '\0';

	if (strncmp(path, "/cygdrive/", 10) == 0 &&
	    isalpha((u_char)path[10]) &&
	    (path[11] == '/' || path[11] == '\0')) {
		drive = path[10];
		rest = path + 11;
	} else if (path[0] == '/' && isalpha((u_char)path[1]) &&
	    (path[2] == '/' || path[2] == '\0')) {
		drive = path[1];
		rest = path + 2;
	}
	if (drive != '\0') {
		xasprintf(&out, "%c:%s", toupper((u_char)drive),
		    *rest == '\0' ? "\\" : rest);
	} else if (isalpha((u_char)path[0]) && path[1] == '\\' &&
	    path[2] == ':') {
		/* MSYS argument conversion can leave an escaped colon. */
		xasprintf(&out, "%c:%s", path[0], path + 3);
	} else
		out = xstrdup(path);

	for (cp = out; *cp != '\0'; cp++) {
		if (*cp == '/')
			*cp = '\\';
	}
	return (out);
}

/* Is this an existing directory? */
static int
win32_is_dir(const wchar_t *path)
{
	DWORD	attr;

	attr = GetFileAttributesW(path);
	return (attr != INVALID_FILE_ATTRIBUTES &&
	    (attr & FILE_ATTRIBUTE_DIRECTORY));
}

/* Is this an existing file? */
static int
win32_is_file(const wchar_t *path)
{
	DWORD	attr;

	attr = GetFileAttributesW(path);
	return (attr != INVALID_FILE_ATTRIBUTES &&
	    (~attr & FILE_ATTRIBUTE_DIRECTORY));
}

/*
 * The current directory of a process is limited to MAX_PATH characters
 * (less the trailing backslash), so try the short name of a longer path.
 * Returns NULL if the directory cannot be used.
 */
static wchar_t *
win32_spawn_cwd_short(wchar_t *path)
{
	wchar_t	*s;
	DWORD	 n;

	if (wcslen(path) < MAX_PATH - 1)
		return (path);
	n = GetShortPathNameW(path, NULL, 0);
	if (n != 0) {
		s = xreallocarray(NULL, n, sizeof *s);
		if (GetShortPathNameW(path, s, n) < n) {
			/*
			 * Long paths can come back as \\?\C:\..., which
			 * cmd.exe takes for a UNC path.
			 */
			if (wcsncmp(s, L"\\\\?\\", 4) == 0 && s[4] != L'\0' &&
			    s[5] == L':')
				wmemmove(s, s + 4, wcslen(s + 4) + 1);
			if (wcslen(s) < MAX_PATH - 1) {
				free(path);
				return (s);
			}
		}
		free(s);
	}
	free(path);
	SetLastError(ERROR_FILENAME_EXCED_RANGE);
	return (NULL);
}

/* Find a usable working directory, falling back like Unix does. */
static wchar_t *
win32_spawn_cwd(const char *cwd, char **cause)
{
	const char	*try[3];
	char		*path, root[4] = "C:\\";
	wchar_t		*w;
	DWORD		 error = ERROR_PATH_NOT_FOUND;
	u_int		 i;
	char		 drive[MAX_PATH];

	if (GetEnvironmentVariableA("SystemDrive", drive, sizeof drive) == 2)
		root[0] = drive[0];

	try[0] = cwd;
	try[1] = find_home();
	try[2] = root;
	for (i = 0; i < nitems(try); i++) {
		if (try[i] == NULL || *try[i] == '\0')
			continue;
		path = win32_path_from_posix(try[i]);
		w = win32_to_wide(path);
		free(path);
		if (win32_is_dir(w)) {
			w = win32_spawn_cwd_short(w);
			if (w != NULL) {
				if (i != 0)
					log_debug("%s: using %s not %s",
					    __func__, try[i], cwd);
				return (w);
			}
		} else
			free(w);
		if (i == 0 && (error = GetLastError()) == ERROR_SUCCESS)
			error = ERROR_DIRECTORY;
	}
	path = win32_error_string(error);
	xasprintf(cause, "%s: %s", cwd, path);
	free(path);
	return (NULL);
}

/* Look up an environment variable in the child or our environment. */
static wchar_t *
win32_spawn_getenv(struct environ *env, const char *name)
{
	struct environ_entry	*envent;
	wchar_t			*wname, *value;
	DWORD			 n;

	if (env != NULL) {
		envent = environ_find(env, name);
		if (envent == NULL || envent->value == NULL)
			return (NULL);
		return (win32_to_wide(envent->value));
	}

	wname = win32_to_wide(name);
	n = GetEnvironmentVariableW(wname, NULL, 0);
	if (n == 0) {
		free(wname);
		return (NULL);
	}
	value = xreallocarray(NULL, n, sizeof *value);
	if (GetEnvironmentVariableW(wname, value, n) >= n) {
		free(value);
		value = NULL;
	}
	free(wname);
	return (value);
}

/* Does the final path component of path have an extension? */
static const wchar_t *
win32_path_extension(const wchar_t *path)
{
	const wchar_t	*cp, *base = path;

	for (cp = path; *cp != L'\0'; cp++) {
		if (*cp == L'\\' || *cp == L'/' || *cp == L':')
			base = cp + 1;
	}
	cp = wcsrchr(base, L'.');
	if (cp == NULL || cp == base || cp[1] == L'\0')
		return (NULL);
	return (cp);
}

/* Try a candidate program name with each PATHEXT extension. */
static wchar_t *
win32_try_program(const wchar_t *dir, const wchar_t *name,
    const wchar_t *pathext)
{
	struct wbuf	 b = { 0 };
	const wchar_t	*ext, *end;
	size_t		 base;

	if (dir != NULL && *dir != L'\0') {
		wbuf_add(&b, dir);
		if (b.s[b.len - 1] != L'\\' && b.s[b.len - 1] != L'/')
			wbuf_addc(&b, L'\\');
	}
	wbuf_add(&b, name);
	base = b.len;

	if (win32_path_extension(name) != NULL && win32_is_file(b.s))
		return (b.s);

	for (ext = pathext; *ext != L'\0'; ext = end) {
		end = wcschr(ext, L';');
		if (end == NULL)
			end = ext + wcslen(ext);
		if (end != ext && *ext == L'.') {
			b.len = base;
			wbuf_addn(&b, ext, end - ext);
			if (win32_is_file(b.s))
				return (b.s);
		}
		if (*end == L';')
			end++;
	}
	free(b.s);
	return (NULL);
}

/* Compare the final component of a path, ignoring case and .exe. */
static int
win32_program_is(const wchar_t *path, const wchar_t *name)
{
	const wchar_t	*base = path, *cp;
	size_t		 len;

	for (cp = path; *cp != L'\0'; cp++) {
		if (*cp == L'\\' || *cp == L'/' || *cp == L':')
			base = cp + 1;
	}
	len = wcslen(base);
	if (len > 4 && _wcsicmp(base + len - 4, L".exe") == 0)
		len -= 4;
	return (len == wcslen(name) && _wcsnicmp(base, name, len) == 0);
}

/* Find cmd.exe, the system shell. */
static wchar_t *
win32_find_cmd(void)
{
	wchar_t	*comspec, sysdir[MAX_PATH], *found;
	UINT	 n;

	comspec = win32_spawn_getenv(NULL, "ComSpec");
	if (comspec != NULL && win32_is_file(comspec))
		return (comspec);
	free(comspec);

	n = GetSystemDirectoryW(sysdir, nitems(sysdir));
	if (n != 0 && n < nitems(sysdir)) {
		found = xcalloc(n + sizeof "\\cmd.exe", sizeof *found);
		wcscpy(found, sysdir);
		wcscat(found, L"\\cmd.exe");
		if (win32_is_file(found))
			return (found);
		free(found);
	}
	return (_wcsdup(L"cmd.exe"));
}

/*
 * Find a program like the shell does: names with a directory are used as
 * given (relative to the child's directory), others are searched for in
 * the child's PATH, trying each PATHEXT extension. The system directories
 * are tried last so programs there are found even with an odd PATH, and
 * cmd.exe is always the system one.
 */
static wchar_t *
win32_find_program(const char *name, struct environ *env, const wchar_t *cwd)
{
	wchar_t		*wname, *path, *pathext, *found = NULL, *dir, *end;
	wchar_t		 sysdir[MAX_PATH];
	const wchar_t	*defext = L".COM;.EXE;.BAT;.CMD";
	char		*converted;
	struct wbuf	 b = { 0 };
	int		 has_dir;
	UINT		 n;

	converted = win32_path_from_posix(name);
	wname = win32_to_wide(converted);
	free(converted);
	has_dir = (wcschr(wname, L'\\') != NULL || wcschr(wname, L':') != NULL);
	if (!has_dir && win32_program_is(wname, L"cmd")) {
		free(wname);
		return (win32_find_cmd());
	}

	pathext = win32_spawn_getenv(env, "PATHEXT");
	if (pathext == NULL || *pathext == L'\0') {
		free(pathext);
		pathext = xcalloc(wcslen(defext) + 1, sizeof *pathext);
		wcscpy(pathext, defext);
	}

	if (has_dir) {
		if (cwd != NULL && wname[0] != L'\\' && wname[1] != L':')
			found = win32_try_program(cwd, wname, pathext);
		else
			found = win32_try_program(NULL, wname, pathext);
		goto out;
	}

	path = win32_spawn_getenv(env, "PATH");
	if (path != NULL) {
		for (dir = path; found == NULL && *dir != L'\0'; dir = end) {
			end = wcschr(dir, L';');
			if (end == NULL)
				end = dir + wcslen(dir);
			b.len = 0;
			wbuf_addn(&b, dir, end - dir);
			if (b.s != NULL && b.s[0] == L'"') {
				wmemmove(b.s, b.s + 1, b.len);
				b.len--;
				if (b.len > 0 && b.s[b.len - 1] == L'"')
					b.s[--b.len] = L'\0';
			}
			if (b.len != 0)
				found = win32_try_program(b.s, wname, pathext);
			if (*end == L';')
				end++;
		}
		free(path);
		free(b.s);
	}
	if (found == NULL) {
		n = GetSystemDirectoryW(sysdir, nitems(sysdir));
		if (n != 0 && n < nitems(sysdir))
			found = win32_try_program(sysdir, wname, pathext);
	}
	if (found == NULL) {
		n = GetWindowsDirectoryW(sysdir, nitems(sysdir));
		if (n != 0 && n < nitems(sysdir))
			found = win32_try_program(sysdir, wname, pathext);
	}

out:
	free(pathext);
	free(wname);
	return (found);
}

/* Is this a batch file that has to be run with cmd.exe? */
static int
win32_program_is_batch(const wchar_t *path)
{
	const wchar_t	*ext;

	ext = win32_path_extension(path);
	return (ext != NULL &&
	    (_wcsicmp(ext, L".bat") == 0 || _wcsicmp(ext, L".cmd") == 0));
}

/* Can this file be started directly by CreateProcess? */
static int
win32_program_is_executable(const wchar_t *path)
{
	const wchar_t	*ext;

	ext = win32_path_extension(path);
	return (ext != NULL &&
	    (_wcsicmp(ext, L".exe") == 0 || _wcsicmp(ext, L".com") == 0));
}

/*
 * Is this an MSYS2 or Cygwin program? Those split their command line with
 * the Cygwin runtime's rules rather than the C runtime's. Git for Windows'
 * bin\bash.exe is a launcher for usr\bin\bash.exe, so look there too.
 */
static int
win32_program_is_cygwin(const wchar_t *path)
{
	static const wchar_t	*dlls[] = {
		L"msys-2.0.dll", L"cygwin1.dll", L"..\\usr\\bin\\msys-2.0.dll"
	};
	struct wbuf		 b = { 0 };
	const wchar_t		*cp, *base = NULL;
	size_t			 dirlen;
	u_int			 i;
	int			 found = 0;

	for (cp = path; *cp != L'\0'; cp++) {
		if (*cp == L'\\' || *cp == L'/')
			base = cp + 1;
	}
	if (base == NULL)
		return (0);
	wbuf_addn(&b, path, base - path);
	dirlen = b.len;
	for (i = 0; i < nitems(dlls) && !found; i++) {
		b.len = dirlen;
		wbuf_add(&b, dlls[i]);
		found = win32_is_file(b.s);
	}
	free(b.s);
	return (found);
}

/* Quote the first argument: it is split off without escapes by everyone. */
static void
win32_quote_argv0(struct wbuf *b, const wchar_t *arg)
{
	wbuf_addc(b, L'"');
	wbuf_add(b, arg);
	wbuf_addc(b, L'"');
}

/* Quote an argument for CommandLineToArgvW and the Microsoft C runtime. */
static void
win32_quote_msvcrt(struct wbuf *b, const wchar_t *arg)
{
	const wchar_t	*cp;
	size_t		 backslashes = 0;

	/* Quote * and ? too so wildcard expansion (setargv) is not done. */
	if (*arg != L'\0' && wcspbrk(arg, L" \t\n\v\"*?") == NULL) {
		wbuf_add(b, arg);
		return;
	}

	wbuf_addc(b, L'"');
	for (cp = arg; *cp != L'\0'; cp++) {
		if (*cp == L'\\') {
			backslashes++;
			continue;
		}
		if (*cp == L'"') {
			/* Escape the backslashes and the quote. */
			wbuf_addcn(b, L'\\', backslashes * 2 + 1);
		} else
			wbuf_addcn(b, L'\\', backslashes);
		backslashes = 0;
		wbuf_addc(b, *cp);
	}
	/* Backslashes before the closing quote must be doubled. */
	wbuf_addcn(b, L'\\', backslashes * 2);
	wbuf_addc(b, L'"');
}

/*
 * Quote an argument for the Cygwin runtime. Quoting stops it expanding
 * wildcards, braces and ~, and inside double quotes it reads \\ as \ and
 * \" as ", so escaping both makes any string come through unchanged.
 */
static void
win32_quote_cygwin(struct wbuf *b, const wchar_t *arg)
{
	const wchar_t	*cp;

	wbuf_addc(b, L'"');
	for (cp = arg; *cp != L'\0'; cp++) {
		if (*cp == L'\\' || *cp == L'"')
			wbuf_addc(b, L'\\');
		wbuf_addc(b, *cp);
	}
	wbuf_addc(b, L'"');
}

/*
 * Quote an argument for a batch file run with cmd.exe /c. cmd.exe expands
 * %VAR% even inside quotes, so break up % with an empty substring of %cd%,
 * and quote everything that is not known to be safe. This follows the
 * hardened rules of Rust's std::process.
 */
static int
win32_quote_batch(struct wbuf *b, const wchar_t *arg)
{
	const wchar_t	*cp;
	size_t		 len = wcslen(arg), backslashes = 0;
	int		 quote = 0;

	if (wcspbrk(arg, L"\r\n") != NULL)
		return (-1);
	if (len == 0 || arg[len - 1] == L'\\')
		quote = 1;
	for (cp = arg; *cp != L'\0'; cp++) {
		if (*cp < 0x80 && !iswalnum(*cp) &&
		    wcschr(L"#$*+-./:?@\\_", *cp) == NULL)
			quote = 1;
		if (*cp < 0x20 || *cp == 0x7f)
			quote = 1;
	}

	if (quote)
		wbuf_addc(b, L'"');
	for (cp = arg; *cp != L'\0'; cp++) {
		if (*cp == L'\\') {
			backslashes++;
			wbuf_addc(b, *cp);
			continue;
		}
		if (*cp == L'"') {
			/* Double the backslashes before and the quote itself. */
			wbuf_addcn(b, L'\\', backslashes);
			wbuf_addc(b, L'"');
		} else if (*cp == L'%')
			wbuf_add(b, L"%%cd:~,");
		backslashes = 0;
		wbuf_addc(b, *cp);
	}
	if (quote) {
		wbuf_addcn(b, L'\\', backslashes);
		wbuf_addc(b, L'"');
	}
	return (0);
}

/*
 * A job has a console of its own without a window, which has the OEM code
 * page (such as 949). cmd.exe and other console programs use that for what
 * they write to a pipe, but tmux wants UTF-8. The code page is per console
 * and must be changed before cmd.exe starts, because it reads it once, and
 * the console only exists once the child is running. So run the command as
 *
 *	cmd /d /v:on /s /c "chcp 65001>nul & cmd /s /c !TMUX_WIN32_JOB!"
 *
 * with "command" in TMUX_WIN32_JOB: delayed expansion is done after the
 * line has been parsed, so the second cmd.exe gets exactly the command line
 * it would otherwise have had, and its exit status is the one returned.
 * Returns 1 if it did.
 */
static int
win32_cmdline_utf8(struct win32_spawn *sp, struct wbuf *b)
{
	wchar_t	sysdir[MAX_PATH], *chcp;
	UINT	n;
	int	found;

	if (~sp->flags & WIN32_SPAWN_UTF8 || GetOEMCP() == CP_UTF8)
		return (0);
	n = GetSystemDirectoryW(sysdir, nitems(sysdir));
	if (n == 0 || n >= nitems(sysdir))
		return (0);
	chcp = xcalloc(n + sizeof "\\chcp.com", sizeof *chcp);
	wcscpy(chcp, sysdir);
	wcscat(chcp, L"\\chcp.com");
	found = win32_is_file(chcp);
	if (found) {
		wbuf_add(b, L" /d /v:on /s /c \"");
		win32_quote_argv0(b, chcp);
		wbuf_add(b, L" 65001>nul 2>nul & ");
		win32_quote_argv0(b, sp->app);
		wbuf_add(b, L" /s /c !" L"" WIN32_JOB_VARIABLE L"!\"");
	}
	free(chcp);
	return (found);
}

/* Build the command line to run a shell command with a shell. */
static int
win32_cmdline_shell(struct win32_spawn *sp, struct wbuf *b, int *wrapped,
    char **cause)
{
	wchar_t	*wcmd;

	sp->app = win32_find_program(sp->shell, sp->env, sp->wcwd);
	if (sp->app == NULL) {
		xasprintf(cause, "shell not found: %s", sp->shell);
		return (-1);
	}
	win32_quote_argv0(b, sp->app);
	if (sp->cmd == NULL)
		return (0);	/* login shell */

	if (win32_program_is(sp->app, L"cmd") && win32_cmdline_utf8(sp, b)) {
		*wrapped = 1;
		return (0);
	}

	wcmd = win32_to_wide(sp->cmd);
	if (win32_program_is(sp->app, L"cmd")) {
		/*
		 * With /s, cmd.exe removes the first and last quotes and runs
		 * what is between them exactly as written.
		 */
		wbuf_add(b, L" /s /c \"");
		wbuf_add(b, wcmd);
		wbuf_addc(b, L'"');
	} else {
		/* sh, bash, zsh, fish, pwsh, powershell, nu, ...: -c. */
		wbuf_add(b, L" -c ");
		if (win32_program_is_cygwin(sp->app))
			win32_quote_cygwin(b, wcmd);
		else
			win32_quote_msvcrt(b, wcmd);
	}
	free(wcmd);
	return (0);
}

/* Build the command line to run an argument vector. */
static int
win32_cmdline_argv(struct win32_spawn *sp, struct wbuf *b, char **cause)
{
	wchar_t	*path, *warg;
	char	*error;
	int	 i, cygwin;

	path = win32_find_program(sp->argv[0], sp->env, sp->wcwd);
	if (path == NULL) {
		error = win32_error_string(ERROR_FILE_NOT_FOUND);
		xasprintf(cause, "%s: %s", sp->argv[0], error);
		free(error);
		return (-1);
	}

	if (!win32_program_is_executable(path)) {
		/*
		 * Batch files (such as npm's .cmd shims) and other scripts
		 * found through PATHEXT are run by cmd.exe as if typed.
		 */
		sp->app = win32_find_cmd();
		win32_quote_argv0(b, sp->app);
		wbuf_add(b, L" /e:on /v:off /d /s /c \"");
		win32_quote_argv0(b, path);
		for (i = 1; i < sp->argc; i++) {
			wbuf_addc(b, L' ');
			warg = win32_to_wide(sp->argv[i]);
			if (win32_quote_batch(b, warg) != 0) {
				free(warg);
				free(path);
				xasprintf(cause, "%s: argument %d contains a "
				    "newline", sp->argv[0], i);
				return (-1);
			}
			free(warg);
		}
		wbuf_addc(b, L'"');
		free(path);
		return (0);
	}

	sp->app = path;
	cygwin = win32_program_is_cygwin(path);
	warg = win32_to_wide(sp->argv[0]);
	if (wcschr(warg, L'"') != NULL)
		win32_quote_argv0(b, path);
	else
		win32_quote_argv0(b, warg);
	free(warg);
	for (i = 1; i < sp->argc; i++) {
		wbuf_addc(b, L' ');
		warg = win32_to_wide(sp->argv[i]);
		if (cygwin)
			win32_quote_cygwin(b, warg);
		else
			win32_quote_msvcrt(b, warg);
		free(warg);
	}
	return (0);
}

/* Is this an environment variable name Windows can store? */
static int
win32_environ_valid(const char *name)
{
	return (*name != '\0' && strchr(name + 1, '=') == NULL);
}

/*
 * Build a CREATE_UNICODE_ENVIRONMENT block. Variables hidden with
 * set-environment -h are left out as on Unix. Names are case-insensitive
 * on Windows and the environ tree is ordered that way, so each name appears
 * once and the block is sorted as CreateProcess expects.
 */
wchar_t *
win32_environ_block(struct environ *env)
{
	struct environ_entry	*envent;
	struct wbuf		 b = { 0 };
	const char		*last = NULL;
	wchar_t			*w;

	for (envent = environ_first(env); envent != NULL;
	    envent = environ_next(envent)) {
		if (envent->value == NULL ||
		    (envent->flags & ENVIRON_HIDDEN) ||
		    !win32_environ_valid(envent->name))
			continue;
		if (last != NULL && _stricmp(last, envent->name) == 0)
			continue;
		last = envent->name;

		w = win32_to_wide(envent->name);
		wbuf_add(&b, w);
		free(w);
		wbuf_addc(&b, L'=');
		w = win32_to_wide(envent->value);
		wbuf_add(&b, w);
		free(w);
		wbuf_addc(&b, L'\0');
	}
	wbuf_addc(&b, L'\0');
	return (b.s);
}

/* Make an environ from this process's environment. */
static struct environ *
win32_environ_current(void)
{
	struct environ	*env = environ_create();
	wchar_t		*block, *w;
	char		*s;

	block = GetEnvironmentStringsW();
	if (block == NULL)
		return (env);
	for (w = block; *w != L'\0'; w += wcslen(w) + 1) {
		if (*w == L'=')
			continue;	/* =C:=C:\dir and the like */
		s = win32_from_wide(w);
		environ_put(env, s, 0);
		free(s);
	}
	FreeEnvironmentStringsW(block);
	return (env);
}

/* Work out the program, command line, environment and directory to use. */
int
win32_spawn_prepare(struct win32_spawn *sp, char **cause)
{
	struct wbuf	 b = { 0 };
	struct environ	*env = NULL;
	char		*cwd;
	int		 error, wrapped = 0;

	*cause = NULL;
	sp->app = sp->cmdline = sp->envblock = sp->wcwd = NULL;

	if (sp->cwd != NULL && *sp->cwd != '\0') {
		sp->wcwd = win32_spawn_cwd(sp->cwd, cause);
		if (sp->wcwd == NULL)
			return (-1);
		if (sp->env != NULL) {
			cwd = win32_from_wide(sp->wcwd);
			environ_set(sp->env, "PWD", 0, "%s", cwd);
			free(cwd);
		}
	}

	if (sp->cmd == NULL && sp->argc > 0)
		error = win32_cmdline_argv(sp, &b, cause);
	else
		error = win32_cmdline_shell(sp, &b, &wrapped, cause);
	if (error != 0) {
		free(b.s);
		win32_spawn_free(sp);
		return (-1);
	}
	sp->cmdline = b.s;

	if (wrapped) {
		/* The command is passed in the environment, so one is needed. */
		if (sp->env == NULL)
			sp->env = env = win32_environ_current();
		environ_set(sp->env, WIN32_JOB_VARIABLE, 0, "\"%s\"", sp->cmd);
	}
	if (sp->env != NULL)
		sp->envblock = win32_environ_block(sp->env);
	if (env != NULL) {
		environ_free(env);
		sp->env = NULL;
	}
	return (0);
}

/* Free what win32_spawn_prepare allocated. */
void
win32_spawn_free(struct win32_spawn *sp)
{
	free(sp->app);
	free(sp->cmdline);
	free(sp->envblock);
	free(sp->wcwd);
	sp->app = sp->cmdline = sp->envblock = sp->wcwd = NULL;
}
