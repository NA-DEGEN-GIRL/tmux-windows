/*
 * IPC layer for Windows.
 *
 * libevent's select() backend can only wait on sockets, so the server keeps
 * its clients on loopback TCP connections and publishes the port through a
 * named pipe, \\.\pipe\<label>:
 *
 * - The server creates the first instance of the pipe itself
 *   (FILE_FLAG_FIRST_PIPE_INSTANCE), owned by the current user and with a
 *   protected DACL admitting only that user, and rejects remote clients.
 *
 * - A client opens the pipe at identification level only and checks it
 *   really belongs to a server run by the same user (owner, DACL, integrity
 *   label and the server process token) before trusting anything it says.
 *
 * - The pipe answers "<port>:<nonce>:<proof>\n" (or "busy\n" if too many
 *   nonces are outstanding). The client connects to 127.0.0.1:<port>, sends
 *   "<nonce>\n" (or "TTY:<token>:<nonce>\n" for the terminal channel) and
 *   waits for "<proof>\n" before speaking the imsg protocol, so each side
 *   knows the other learnt the secrets from the protected pipe.
 *
 * The TCP listener uses SO_EXCLUSIVEADDRUSE so no other account can bind the
 * same port, and every socket and handle is created non-inheritable so that
 * child processes never hold client connections open.
 */

#include <sys/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

#include <aclapi.h>
#include <sddl.h>

/*
 * RtlGenRandom is exported as SystemFunction036 from advapi32.dll. It is the
 * recommended way to generate cryptographic random bytes without pulling in
 * the full BCrypt API.
 */
#define RtlGenRandom SystemFunction036
BOOLEAN NTAPI RtlGenRandom(PVOID, ULONG);

#define IPC_TOKEN_BYTES		32
#define IPC_TOKEN_LEN		(IPC_TOKEN_BYTES * 2)
#define IPC_MAX_NONCES		256
#define IPC_NONCE_EXPIRY	30000	/* milliseconds */
#define IPC_PIPE_INSTANCES	4
#define IPC_PIPE_TIMEOUT	5000	/* milliseconds */
#define IPC_PIPE_RETRY		1000	/* milliseconds */
#define IPC_CONNECT_TIMEOUT	10000	/* milliseconds */
#define IPC_REPLY_TIMEOUT	5000	/* milliseconds */
#define IPC_LINE_MAX		192
#define IPC_LOCK_TIMEOUT	15000	/* milliseconds */

/* Outstanding nonces handed out through the pipe. */
struct ipc_nonce {
	char		nonce[IPC_TOKEN_LEN + 1];
	char		proof[IPC_TOKEN_LEN + 1];
	ULONGLONG	created;
	int		used;
};
static struct ipc_nonce		ipc_nonces[IPC_MAX_NONCES];
static CRITICAL_SECTION		ipc_nonce_lock;
static int			ipc_nonce_lock_init;

/* Server pipe instances, all served by one thread. */
enum ipc_pipe_state {
	IPC_PIPE_CONNECTING,
	IPC_PIPE_WRITING,
	IPC_PIPE_DRAINING,
	IPC_PIPE_BROKEN
};
struct ipc_pipe {
	HANDLE			 handle;
	OVERLAPPED		 ov;
	enum ipc_pipe_state	 state;
	int			 pending;
	ULONGLONG		 deadline;
	int			 slot;
	char			 nonce[IPC_TOKEN_LEN + 1];
	char			 out[IPC_LINE_MAX];
	DWORD			 outlen;
	char			 in[64];
};
static struct ipc_pipe		 ipc_pipes[IPC_PIPE_INSTANCES];
static u_int			 ipc_npipes;
static wchar_t			*ipc_pipe_name;
static PSECURITY_DESCRIPTOR	 ipc_pipe_sd;
static HANDLE			 ipc_thread;
static HANDLE			 ipc_shutdown;
static u_short			 ipc_port;

static TOKEN_USER		*ipc_token_user;

static void	ipc_pipe_listen(struct ipc_pipe *);

/* Convert a UTF-8 string (or, failing that, an ANSI one) to UTF-16. */
wchar_t *
win32_ipc_utf8_to_wide(const char *s)
{
	wchar_t	*w;
	int	 n;
	UINT	 cp = CP_UTF8;
	DWORD	 flags = MB_ERR_INVALID_CHARS;

	n = MultiByteToWideChar(cp, flags, s, -1, NULL, 0);
	if (n == 0) {
		cp = CP_ACP;
		flags = 0;
		n = MultiByteToWideChar(cp, flags, s, -1, NULL, 0);
		if (n == 0)
			return (NULL);
	}
	w = xreallocarray(NULL, n, sizeof *w);
	if (MultiByteToWideChar(cp, flags, s, -1, w, n) == 0) {
		free(w);
		return (NULL);
	}
	return (w);
}

/* Convert a UTF-16 string to UTF-8. */
char *
win32_ipc_wide_to_utf8(const wchar_t *w)
{
	char	*s;
	int	 n;

	n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (n == 0)
		return (NULL);
	s = xmalloc(n);
	if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) == 0) {
		free(s);
		return (NULL);
	}
	return (s);
}

/* Describe a Windows error code. */
char *
win32_ipc_strerror(DWORD error)
{
	wchar_t	 buf[512];
	char	*s, *cp;
	DWORD	 flags;

	flags = FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS;
	if (FormatMessageW(flags, NULL, error, 0, buf, nitems(buf), NULL) == 0 ||
	    (s = win32_ipc_wide_to_utf8(buf)) == NULL) {
		xasprintf(&s, "Windows error %lu", (u_long)error);
		return (s);
	}
	cp = s + strlen(s);
	while (cp > s && (cp[-1] == '\n' || cp[-1] == '\r' || cp[-1] == ' ' ||
	    cp[-1] == '.'))
		*--cp = '\0';
	return (s);
}

/* Get the pipe name for a label. */
static wchar_t *
ipc_pipe_path(const char *label)
{
	wchar_t	*w, *name;
	size_t	 len;

	if ((w = win32_ipc_utf8_to_wide(label)) == NULL)
		return (NULL);
	len = wcslen(w) + 10;
	name = xreallocarray(NULL, len, sizeof *name);
	_snwprintf_s(name, len, _TRUNCATE, L"\\\\.\\pipe\\%ls", w);
	free(w);
	return (name);
}

/* Get the current user's SID. */
static PSID
ipc_user_sid(void)
{
	HANDLE		 token;
	DWORD		 len = 0;
	TOKEN_USER	*tu;

	if (ipc_token_user != NULL)
		return (ipc_token_user->User.Sid);

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return (NULL);
	GetTokenInformation(token, TokenUser, NULL, 0, &len);
	if (len == 0) {
		CloseHandle(token);
		return (NULL);
	}
	tu = xmalloc(len);
	if (!GetTokenInformation(token, TokenUser, tu, len, &len)) {
		free(tu);
		CloseHandle(token);
		return (NULL);
	}
	CloseHandle(token);

	ipc_token_user = tu;
	return (tu->User.Sid);
}

/* Get the integrity level RID of a token. */
static DWORD
ipc_token_integrity(HANDLE token)
{
	TOKEN_MANDATORY_LABEL	*tml;
	DWORD			 len = 0, rid;
	PSID			 sid;

	GetTokenInformation(token, TokenIntegrityLevel, NULL, 0, &len);
	if (len == 0)
		return (SECURITY_MANDATORY_MEDIUM_RID);
	tml = xmalloc(len);
	if (!GetTokenInformation(token, TokenIntegrityLevel, tml, len, &len)) {
		free(tml);
		return (SECURITY_MANDATORY_MEDIUM_RID);
	}
	sid = tml->Label.Sid;
	rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
	free(tml);
	return (rid);
}

/* Get the integrity level RID of this process. */
static DWORD
ipc_own_integrity(void)
{
	HANDLE	token;
	DWORD	rid;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return (SECURITY_MANDATORY_MEDIUM_RID);
	rid = ipc_token_integrity(token);
	CloseHandle(token);
	return (rid);
}

/*
 * Build a security descriptor owned by and granting access only to the
 * current user. Free with LocalFree.
 */
static PSECURITY_DESCRIPTOR
ipc_owner_only_sd(void)
{
	PSID			 sid;
	char			*sid_str, *sddl;
	PSECURITY_DESCRIPTOR	 sd = NULL;

	if ((sid = ipc_user_sid()) == NULL)
		return (NULL);
	if (!ConvertSidToStringSidA(sid, &sid_str))
		return (NULL);
	xasprintf(&sddl, "O:%sD:P(A;;GA;;;%s)", sid_str, sid_str);
	LocalFree(sid_str);

	if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl,
	    SDDL_REVISION_1, &sd, NULL))
		sd = NULL;
	free(sddl);
	return (sd);
}

/* Fill a buffer with a random hex token. */
static int
ipc_random_token(char *out)
{
	static const char	hex[] = "0123456789abcdef";
	u_char			bytes[IPC_TOKEN_BYTES];
	int			i;

	if (!RtlGenRandom(bytes, sizeof bytes))
		return (-1);
	for (i = 0; i < IPC_TOKEN_BYTES; i++) {
		out[i * 2] = hex[bytes[i] >> 4];
		out[i * 2 + 1] = hex[bytes[i] & 0xf];
	}
	out[IPC_TOKEN_LEN] = '\0';
	SecureZeroMemory(bytes, sizeof bytes);
	return (0);
}

/* Is this a well-formed token? */
static int
ipc_valid_token(const char *s)
{
	size_t	i;

	for (i = 0; i < IPC_TOKEN_LEN; i++) {
		if ((s[i] < '0' || s[i] > '9') && (s[i] < 'a' || s[i] > 'f'))
			return (0);
	}
	return (s[i] == '\0');
}

/* Compare two well-formed tokens in constant time. */
static int
ipc_token_equal(const char *a, const char *b)
{
	u_char	diff = 0;
	size_t	i;

	for (i = 0; i < IPC_TOKEN_LEN; i++)
		diff |= (u_char)a[i] ^ (u_char)b[i];
	return (diff == 0);
}

/* Generate a random token for the tty channel. */
void
win32_generate_tty_token(char *buf, size_t bufsize)
{
	if (bufsize < IPC_TOKEN_LEN + 1 || ipc_random_token(buf) != 0)
		*buf = '\0';
}

/* Add a nonce and its proof. Returns the slot or -1 if the table is full. */
static int
ipc_add_nonce(char *nonce, char *proof)
{
	struct ipc_nonce	*n;
	ULONGLONG		 now;
	int			 i, slot = -1;

	if (ipc_random_token(nonce) != 0 || ipc_random_token(proof) != 0)
		return (-1);

	EnterCriticalSection(&ipc_nonce_lock);
	now = GetTickCount64();
	for (i = 0; i < IPC_MAX_NONCES; i++) {
		n = &ipc_nonces[i];
		if (n->used && now - n->created > IPC_NONCE_EXPIRY) {
			SecureZeroMemory(n, sizeof *n);
			n->used = 0;
		}
		if (!n->used && slot == -1)
			slot = i;
	}
	if (slot != -1) {
		n = &ipc_nonces[slot];
		memcpy(n->nonce, nonce, sizeof n->nonce);
		memcpy(n->proof, proof, sizeof n->proof);
		n->created = now;
		n->used = 1;
	}
	LeaveCriticalSection(&ipc_nonce_lock);
	return (slot);
}

/* Withdraw a nonce that never reached its client. */
static void
ipc_release_nonce(int slot, const char *nonce)
{
	struct ipc_nonce	*n = &ipc_nonces[slot];

	EnterCriticalSection(&ipc_nonce_lock);
	if (n->used && strcmp(n->nonce, nonce) == 0)
		SecureZeroMemory(n, sizeof *n);
	LeaveCriticalSection(&ipc_nonce_lock);
}

/* Look up and consume a nonce. Returns 0 and its proof if found. */
static int
ipc_consume_nonce(const char *nonce, char *proof)
{
	struct ipc_nonce	*n;
	ULONGLONG		 now;
	int			 i, found = -1;

	EnterCriticalSection(&ipc_nonce_lock);
	now = GetTickCount64();
	for (i = 0; i < IPC_MAX_NONCES; i++) {
		n = &ipc_nonces[i];
		if (!n->used)
			continue;
		if (now - n->created > IPC_NONCE_EXPIRY) {
			SecureZeroMemory(n, sizeof *n);
			continue;
		}
		if (ipc_token_equal(n->nonce, nonce) && found == -1)
			found = i;
	}
	if (found != -1) {
		n = &ipc_nonces[found];
		memcpy(proof, n->proof, IPC_TOKEN_LEN + 1);
		SecureZeroMemory(n, sizeof *n);
	}
	LeaveCriticalSection(&ipc_nonce_lock);
	return (found == -1 ? -1 : 0);
}

/* Create one server pipe instance. */
static HANDLE
ipc_create_instance(int first)
{
	SECURITY_ATTRIBUTES	sa;
	DWORD			open_mode;

	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = ipc_pipe_sd;
	sa.bInheritHandle = FALSE;

	open_mode = PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED;
	if (first)
		open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
	return (CreateNamedPipeW(ipc_pipe_name, open_mode,
	    PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|
	    PIPE_REJECT_REMOTE_CLIENTS, IPC_PIPE_INSTANCES, IPC_LINE_MAX,
	    IPC_LINE_MAX, 0, &sa));
}

/* Prepare an instance's OVERLAPPED for a new operation. */
static void
ipc_pipe_prepare(struct ipc_pipe *p)
{
	HANDLE	event = p->ov.hEvent;

	memset(&p->ov, 0, sizeof p->ov);
	p->ov.hEvent = event;
	ResetEvent(event);
}

/* Abandon whatever an instance is doing and start listening again. */
static void
ipc_pipe_reset(struct ipc_pipe *p)
{
	DWORD	n;

	if (p->pending) {
		CancelIoEx(p->handle, &p->ov);
		GetOverlappedResult(p->handle, &p->ov, &n, TRUE);
		p->pending = 0;
	}
	if (p->slot != -1) {
		ipc_release_nonce(p->slot, p->nonce);
		p->slot = -1;
	}
	SecureZeroMemory(p->nonce, sizeof p->nonce);
	SecureZeroMemory(p->out, sizeof p->out);
	DisconnectNamedPipe(p->handle);
	ipc_pipe_listen(p);
}

/* Wait for the client to close its end once it has read the reply. */
static void
ipc_pipe_drain(struct ipc_pipe *p)
{
	ipc_pipe_prepare(p);
	p->state = IPC_PIPE_DRAINING;
	if (!ReadFile(p->handle, p->in, sizeof p->in, NULL, &p->ov) &&
	    GetLastError() != ERROR_IO_PENDING) {
		/* Already closed: ready for the next client. */
		DisconnectNamedPipe(p->handle);
		ipc_pipe_listen(p);
		return;
	}
	p->pending = 1;
}

/* A client has connected to an instance: send it the port and a nonce. */
static void
ipc_pipe_connected(struct ipc_pipe *p)
{
	char	proof[IPC_TOKEN_LEN + 1];
	int	n;

	p->slot = ipc_add_nonce(p->nonce, proof);
	if (p->slot == -1)
		n = snprintf(p->out, sizeof p->out, "busy\n");
	else {
		n = snprintf(p->out, sizeof p->out, "%u:%s:%s\n", ipc_port,
		    p->nonce, proof);
	}
	SecureZeroMemory(proof, sizeof proof);
	p->outlen = (DWORD)n;

	ipc_pipe_prepare(p);
	p->state = IPC_PIPE_WRITING;
	p->deadline = GetTickCount64() + IPC_PIPE_TIMEOUT;
	if (!WriteFile(p->handle, p->out, p->outlen, NULL, &p->ov) &&
	    GetLastError() != ERROR_IO_PENDING) {
		ipc_pipe_reset(p);
		return;
	}
	p->pending = 1;
}

/* Start waiting for a client on an instance. */
static void
ipc_pipe_listen(struct ipc_pipe *p)
{
	DWORD	error;
	int	tries;

	for (tries = 0; tries < 3; tries++) {
		ipc_pipe_prepare(p);
		p->state = IPC_PIPE_CONNECTING;
		p->deadline = 0;
		if (ConnectNamedPipe(p->handle, &p->ov)) {
			ipc_pipe_connected(p);
			return;
		}
		switch (error = GetLastError()) {
		case ERROR_IO_PENDING:
			p->pending = 1;
			return;
		case ERROR_PIPE_CONNECTED:
			ipc_pipe_connected(p);
			return;
		case ERROR_NO_DATA:
			/* The client connected and left already. */
			DisconnectNamedPipe(p->handle);
			continue;
		}
		break;
	}

	/* Try again later; the other instances keep serving. */
	ResetEvent(p->ov.hEvent);
	p->state = IPC_PIPE_BROKEN;
	p->deadline = GetTickCount64() + IPC_PIPE_RETRY;
}

/* An instance's pending operation has finished. */
static void
ipc_pipe_complete(struct ipc_pipe *p)
{
	DWORD	n;
	BOOL	ok;

	if (!p->pending) {
		ResetEvent(p->ov.hEvent);
		return;
	}
	ok = GetOverlappedResult(p->handle, &p->ov, &n, FALSE);
	if (!ok && GetLastError() == ERROR_IO_INCOMPLETE)
		return;
	p->pending = 0;

	switch (p->state) {
	case IPC_PIPE_CONNECTING:
		if (!ok) {
			ipc_pipe_reset(p);
			break;
		}
		ipc_pipe_connected(p);
		break;
	case IPC_PIPE_WRITING:
		if (!ok || n != p->outlen) {
			ipc_pipe_reset(p);
			break;
		}
		SecureZeroMemory(p->out, sizeof p->out);
		p->slot = -1;	/* delivered */
		p->deadline = GetTickCount64() + IPC_PIPE_TIMEOUT;
		ipc_pipe_drain(p);
		break;
	case IPC_PIPE_DRAINING:
		if (!ok) {
			/* Normally ERROR_BROKEN_PIPE: the client closed. */
			DisconnectNamedPipe(p->handle);
			ipc_pipe_listen(p);
			break;
		}
		ipc_pipe_drain(p);
		break;
	case IPC_PIPE_BROKEN:
		break;
	}
}

/* Discovery thread: serves every pipe instance. */
static DWORD WINAPI
ipc_pipe_thread(__unused LPVOID arg)
{
	HANDLE		 events[IPC_PIPE_INSTANCES + 1];
	struct ipc_pipe	*p;
	ULONGLONG	 now;
	DWORD		 timeout, r, n;
	u_int		 i;

	for (i = 0; i < ipc_npipes; i++)
		ipc_pipe_listen(&ipc_pipes[i]);

	for (;;) {
		now = GetTickCount64();
		timeout = INFINITE;
		for (i = 0; i < ipc_npipes; i++) {
			p = &ipc_pipes[i];
			if (p->deadline == 0)
				continue;
			if (p->deadline <= now) {
				if (p->state == IPC_PIPE_BROKEN) {
					DisconnectNamedPipe(p->handle);
					ipc_pipe_listen(p);
				} else
					ipc_pipe_reset(p);
				now = GetTickCount64();
			}
			if (p->deadline != 0 && p->deadline > now &&
			    p->deadline - now < timeout)
				timeout = (DWORD)(p->deadline - now);
		}

		events[0] = ipc_shutdown;
		for (i = 0; i < ipc_npipes; i++)
			events[i + 1] = ipc_pipes[i].ov.hEvent;
		r = WaitForMultipleObjects(ipc_npipes + 1, events, FALSE,
		    timeout);
		if (r == WAIT_OBJECT_0)
			break;
		if (r > WAIT_OBJECT_0 && r <= WAIT_OBJECT_0 + ipc_npipes)
			ipc_pipe_complete(&ipc_pipes[r - WAIT_OBJECT_0 - 1]);
		else if (r == WAIT_FAILED)
			Sleep(100);
	}

	for (i = 0; i < ipc_npipes; i++) {
		p = &ipc_pipes[i];
		if (p->pending) {
			CancelIoEx(p->handle, &p->ov);
			GetOverlappedResult(p->handle, &p->ov, &n, TRUE);
			p->pending = 0;
		}
		DisconnectNamedPipe(p->handle);
		CloseHandle(p->handle);
		CloseHandle(p->ov.hEvent);
		p->handle = INVALID_HANDLE_VALUE;
	}
	ipc_npipes = 0;
	return (0);
}

/* Create a non-inheritable TCP socket. */
static SOCKET
ipc_socket(void)
{
	SOCKET	s;
	BOOL	one = TRUE;

	s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
	    WSA_FLAG_OVERLAPPED|WSA_FLAG_NO_HANDLE_INHERIT);
	if (s == INVALID_SOCKET)
		return (INVALID_SOCKET);
	SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
	return (s);
}

/*
 * Create a server: a loopback TCP listener plus the discovery pipe. Returns
 * the listening socket or -1 with *cause set.
 */
int
win32_ipc_create_server(const char *label, char **cause)
{
	SOCKET			 s;
	struct sockaddr_in	 addr;
	int			 addrlen = sizeof addr;
	BOOL			 one = TRUE;
	struct ipc_pipe		*p;
	DWORD			 error;
	char			*msg;
	u_int			 i;

	win32_wsa_init();
	if (!ipc_nonce_lock_init) {
		InitializeCriticalSection(&ipc_nonce_lock);
		ipc_nonce_lock_init = 1;
	}

	if ((ipc_pipe_name = ipc_pipe_path(label)) == NULL) {
		xasprintf(cause, "bad server name %s", label);
		return (-1);
	}
	if ((ipc_pipe_sd = ipc_owner_only_sd()) == NULL) {
		msg = win32_ipc_strerror(GetLastError());
		xasprintf(cause, "can't build security descriptor (%s)", msg);
		free(msg);
		return (-1);
	}

	/* Loopback listener on an OS-assigned port nobody else can share. */
	if ((s = ipc_socket()) == INVALID_SOCKET)
		goto socket_failed;
	if (setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one,
	    sizeof one) != 0)
		goto socket_failed;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0)
		goto socket_failed;
	if (getsockname(s, (struct sockaddr *)&addr, &addrlen) != 0)
		goto socket_failed;
	if (listen(s, SOMAXCONN) != 0)
		goto socket_failed;
	ipc_port = ntohs(addr.sin_port);

	/* Discovery pipe instances; the first must not already exist. */
	for (i = 0; i < IPC_PIPE_INSTANCES; i++) {
		p = &ipc_pipes[i];
		memset(p, 0, sizeof *p);
		p->slot = -1;
		p->ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
		if (p->ov.hEvent == NULL)
			break;
		p->handle = ipc_create_instance(i == 0);
		if (p->handle != INVALID_HANDLE_VALUE)
			continue;
		error = GetLastError();
		CloseHandle(p->ov.hEvent);
		if (i != 0)
			break;
		if (error == ERROR_ACCESS_DENIED || error == ERROR_PIPE_BUSY) {
			xasprintf(cause, "server name %s is already in use "
			    "(another server, or another program or user, "
			    "owns \\\\.\\pipe\\%s)", label, label);
		} else {
			msg = win32_ipc_strerror(error);
			xasprintf(cause, "error creating server pipe for %s "
			    "(%s)", label, msg);
			free(msg);
		}
		closesocket(s);
		return (-1);
	}
	ipc_npipes = i;
	log_debug("%s: port %u, %u pipe instances", __func__, ipc_port,
	    ipc_npipes);

	ipc_shutdown = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (ipc_shutdown != NULL)
		ipc_thread = CreateThread(NULL, 0, ipc_pipe_thread, NULL, 0,
		    NULL);
	if (ipc_thread == NULL) {
		msg = win32_ipc_strerror(GetLastError());
		xasprintf(cause, "can't start discovery thread (%s)", msg);
		free(msg);
		for (i = 0; i < ipc_npipes; i++) {
			CloseHandle(ipc_pipes[i].handle);
			CloseHandle(ipc_pipes[i].ov.hEvent);
		}
		ipc_npipes = 0;
		closesocket(s);
		return (-1);
	}
	return ((int)s);

socket_failed:
	msg = win32_ipc_strerror(WSAGetLastError());
	xasprintf(cause, "error creating server socket for %s (%s)", label,
	    msg);
	free(msg);
	if (s != INVALID_SOCKET)
		closesocket(s);
	return (-1);
}

/* Shut down the discovery thread and pipe instances (server). */
void
win32_ipc_cleanup(void)
{
	if (ipc_shutdown != NULL)
		SetEvent(ipc_shutdown);
	if (ipc_thread != NULL) {
		WaitForSingleObject(ipc_thread, 3000);
		CloseHandle(ipc_thread);
		ipc_thread = NULL;
	}
	if (ipc_shutdown != NULL) {
		CloseHandle(ipc_shutdown);
		ipc_shutdown = NULL;
	}
}

/*
 * Accept a connection on the listener. Returns the new socket, or -1 with
 * errno EAGAIN for transient failures (including a client that gave up
 * before being accepted), EMFILE when out of resources, or EIO otherwise.
 */
int
win32_ipc_accept(int fd)
{
	SOCKET	s;
	BOOL	one = TRUE;
	int	error;

	s = accept((SOCKET)fd, NULL, NULL);
	if (s == INVALID_SOCKET) {
		switch (error = WSAGetLastError()) {
		case WSAEWOULDBLOCK:
		case WSAEINTR:
		case WSAEINPROGRESS:
		case WSAECONNRESET:
		case WSAECONNABORTED:
		case WSAENETDOWN:
		case WSAETIMEDOUT:
			errno = EAGAIN;
			break;
		case WSAEMFILE:
		case WSAENOBUFS:
			errno = EMFILE;
			break;
		default:
			log_debug("%s: accept failed: %d", __func__, error);
			errno = EIO;
			break;
		}
		return (-1);
	}
	SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
	return ((int)s);
}

/*
 * Read the authentication line from a newly accepted, non-blocking
 * connection. Never reads beyond the line. Returns 1 if more data is needed,
 * 0 once the peer has authenticated (and been sent the proof) or -1 if it
 * must be dropped. *is_tty is set for a terminal channel and its token
 * copied to tty_token.
 */
int
win32_ipc_read_auth(int fd, struct win32_ipc_auth *auth, char *tty_token,
    size_t tty_token_size, int *is_tty)
{
	char	*nl, *nonce, *colon, proof[IPC_TOKEN_LEN + 2];
	int	 n, want;

	*is_tty = 0;
	for (;;) {
		want = (int)(sizeof auth->buf - 1 - auth->len);
		if (want <= 0)
			return (-1);
		n = recv((SOCKET)fd, auth->buf + auth->len, want, MSG_PEEK);
		if (n == SOCKET_ERROR) {
			if (WSAGetLastError() == WSAEWOULDBLOCK)
				return (1);
			return (-1);
		}
		if (n == 0)
			return (-1);
		nl = memchr(auth->buf + auth->len, '\n', n);
		if (nl != NULL)
			want = (int)(nl - (auth->buf + auth->len)) + 1;
		else
			want = n;
		if (recv((SOCKET)fd, auth->buf + auth->len, want, 0) != want)
			return (-1);
		auth->len += want;
		if (nl != NULL)
			break;
	}
	auth->buf[auth->len - 1] = '\0';

	if (strncmp(auth->buf, "TTY:", 4) == 0) {
		colon = strchr(auth->buf + 4, ':');
		if (colon == NULL)
			return (-1);
		*colon = '\0';
		if (!ipc_valid_token(auth->buf + 4) ||
		    tty_token_size < IPC_TOKEN_LEN + 1)
			return (-1);
		strlcpy(tty_token, auth->buf + 4, tty_token_size);
		nonce = colon + 1;
		*is_tty = 1;
	} else
		nonce = auth->buf;
	if (!ipc_valid_token(nonce))
		return (-1);
	if (ipc_consume_nonce(nonce, proof) != 0)
		return (-1);
	SecureZeroMemory(auth, sizeof *auth);

	proof[IPC_TOKEN_LEN] = '\n';
	n = send((SOCKET)fd, proof, IPC_TOKEN_LEN + 1, 0);
	SecureZeroMemory(proof, sizeof proof);
	if (n != IPC_TOKEN_LEN + 1)
		return (-1);
	return (0);
}

/*
 * Check that an opened pipe belongs to a server run by this user: owned by
 * our SID, a DACL that grants nothing to anybody else, an integrity label no
 * lower than ours (up to medium) and, where it can be inspected, a server
 * process running as our user.
 */
static int
ipc_check_pipe(HANDLE pipe, const char *label, char **cause)
{
	PSECURITY_DESCRIPTOR	 sd = NULL;
	PSID			 owner = NULL, user, sid;
	PACL			 dacl = NULL, sacl = NULL;
	ACE_HEADER		*ace;
	DWORD			 error, i, rid, need;
	ULONG			 pid;
	HANDLE			 process, token;
	TOKEN_USER		*tu;
	DWORD			 len;
	int			 bad = 0;
	char			*msg;

	if ((user = ipc_user_sid()) == NULL) {
		xasprintf(cause, "can't get current user");
		return (-1);
	}

	error = GetSecurityInfo(pipe, SE_KERNEL_OBJECT,
	    OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|
	    LABEL_SECURITY_INFORMATION, &owner, NULL, &dacl, &sacl, &sd);
	if (error != ERROR_SUCCESS) {
		msg = win32_ipc_strerror(error);
		xasprintf(cause, "can't check server %s (%s)", label, msg);
		free(msg);
		return (-1);
	}

	if (owner == NULL || !EqualSid(owner, user)) {
		xasprintf(cause, "server %s is not owned by the current user; "
		    "refusing to connect", label);
		goto fail;
	}

	if (dacl == NULL)
		bad = 1;
	for (i = 0; !bad && i < dacl->AceCount; i++) {
		if (!GetAce(dacl, i, (void **)&ace)) {
			bad = 1;
			break;
		}
		switch (ace->AceType) {
		case ACCESS_DENIED_ACE_TYPE:
			break;
		case ACCESS_ALLOWED_ACE_TYPE:
			sid = &((ACCESS_ALLOWED_ACE *)ace)->SidStart;
			if (!EqualSid(sid, user))
				bad = 1;
			break;
		default:
			bad = 1;
			break;
		}
	}
	if (bad) {
		xasprintf(cause, "server %s grants access to other users; "
		    "refusing to connect", label);
		goto fail;
	}

	need = ipc_own_integrity();
	if (need > SECURITY_MANDATORY_MEDIUM_RID)
		need = SECURITY_MANDATORY_MEDIUM_RID;
	rid = SECURITY_MANDATORY_MEDIUM_RID;
	for (i = 0; sacl != NULL && i < sacl->AceCount; i++) {
		if (!GetAce(sacl, i, (void **)&ace))
			continue;
		if (ace->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE)
			continue;
		sid = &((SYSTEM_MANDATORY_LABEL_ACE *)ace)->SidStart;
		rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) -
		    1);
	}
	if (rid < need) {
		xasprintf(cause, "server %s was created at a lower integrity "
		    "level; refusing to connect", label);
		goto fail;
	}

	/* The process serving the pipe should be ours as well. */
	if (GetNamedPipeServerProcessId(pipe, &pid) &&
	    (process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
	    pid)) != NULL) {
		if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
			len = 0;
			GetTokenInformation(token, TokenUser, NULL, 0, &len);
			tu = (len != 0) ? xmalloc(len) : NULL;
			if (tu != NULL &&
			    GetTokenInformation(token, TokenUser, tu, len,
			    &len) && !EqualSid(tu->User.Sid, user))
				bad = 1;
			else if (ipc_token_integrity(token) < need)
				bad = 1;
			free(tu);
			CloseHandle(token);
		}
		CloseHandle(process);
		if (bad) {
			xasprintf(cause, "server %s is run by another user or "
			    "at a lower integrity level; refusing to connect",
			    label);
			goto fail;
		}
	}

	LocalFree(sd);
	return (0);

fail:
	LocalFree(sd);
	return (-1);
}

/* Read a line from the discovery pipe with a deadline. */
static int
ipc_pipe_read_line(HANDLE pipe, char *buf, size_t size, ULONGLONG deadline)
{
	OVERLAPPED	 ov;
	size_t		 len = 0;
	DWORD		 n, wait;
	ULONGLONG	 now;
	char		*nl;

	memset(&ov, 0, sizeof ov);
	if ((ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL)) == NULL)
		return (-1);
	for (;;) {
		ResetEvent(ov.hEvent);
		if (!ReadFile(pipe, buf + len, (DWORD)(size - 1 - len), NULL,
		    &ov) && GetLastError() != ERROR_IO_PENDING)
			break;
		now = GetTickCount64();
		wait = (now < deadline) ? (DWORD)(deadline - now) : 0;
		if (WaitForSingleObject(ov.hEvent, wait) != WAIT_OBJECT_0) {
			CancelIoEx(pipe, &ov);
			GetOverlappedResult(pipe, &ov, &n, TRUE);
			break;
		}
		if (!GetOverlappedResult(pipe, &ov, &n, FALSE) || n == 0)
			break;
		len += n;
		buf[len] = '\0';
		if ((nl = strchr(buf, '\n')) != NULL) {
			*nl = '\0';
			CloseHandle(ov.hEvent);
			return (0);
		}
		if (len >= size - 1)
			break;
	}
	CloseHandle(ov.hEvent);
	return (-1);
}

/* Send all of a buffer on a blocking socket. */
static int
ipc_send_all(SOCKET s, const char *buf, size_t len)
{
	int	n;

	while (len > 0) {
		n = send(s, buf, (int)len, 0);
		if (n <= 0)
			return (-1);
		buf += n;
		len -= n;
	}
	return (0);
}

/* Read the server's proof line from a blocking socket with a deadline. */
static int
ipc_recv_line(SOCKET s, char *buf, size_t size, ULONGLONG deadline)
{
	fd_set		 set;
	struct timeval	 tv;
	ULONGLONG	 now;
	size_t		 len = 0;
	char		 ch;

	while (len < size - 1) {
		now = GetTickCount64();
		if (now >= deadline)
			return (-1);
		tv.tv_sec = (long)((deadline - now) / 1000);
		tv.tv_usec = (long)(((deadline - now) % 1000) * 1000);
		FD_ZERO(&set);
		FD_SET(s, &set);
		if (select(0, &set, NULL, NULL, &tv) != 1)
			return (-1);
		if (recv(s, &ch, 1, 0) != 1)
			return (-1);
		if (ch == '\n') {
			buf[len] = '\0';
			return (0);
		}
		buf[len++] = ch;
	}
	return (-1);
}

/*
 * Connect to the server for a label. With a tty token this is the terminal
 * channel of an existing client. Returns an authenticated, blocking socket
 * or -1 with *status set to WIN32_IPC_NOSERVER (nothing is listening) or
 * WIN32_IPC_ERROR (with *cause set).
 */
int
win32_ipc_connect(const char *label, const char *tty_token, int *status,
    char **cause)
{
	wchar_t			*name;
	HANDLE			 pipe;
	ULONGLONG		 deadline, now;
	DWORD			 error, wait;
	int			 sent;
	char			 line[IPC_LINE_MAX], reply[IPC_LINE_MAX];
	char			*nonce, *proof, *end, *msg, *auth;
	u_long			 port;
	SOCKET			 s = INVALID_SOCKET;
	struct sockaddr_in	 addr;

	win32_wsa_init();
	*status = WIN32_IPC_ERROR;
	*cause = NULL;

	if ((name = ipc_pipe_path(label)) == NULL) {
		xasprintf(cause, "bad server name %s", label);
		return (-1);
	}
	deadline = GetTickCount64() + IPC_CONNECT_TIMEOUT;

	for (;;) {
		pipe = CreateFileW(name, GENERIC_READ|GENERIC_WRITE, 0, NULL,
		    OPEN_EXISTING, FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|
		    SECURITY_IDENTIFICATION, NULL);
		if (pipe == INVALID_HANDLE_VALUE) {
			error = GetLastError();
			if (error == ERROR_FILE_NOT_FOUND ||
			    error == ERROR_PATH_NOT_FOUND) {
				*status = WIN32_IPC_NOSERVER;
				goto fail;
			}
			now = GetTickCount64();
			if (error == ERROR_PIPE_BUSY && now < deadline) {
				wait = (DWORD)(deadline - now);
				if (wait > 1000)
					wait = 1000;
				if (!WaitNamedPipeW(name, wait) &&
				    GetLastError() == ERROR_FILE_NOT_FOUND) {
					*status = WIN32_IPC_NOSERVER;
					goto fail;
				}
				continue;
			}
			if (error == ERROR_PIPE_BUSY) {
				xasprintf(cause, "server %s is not responding",
				    label);
			} else if (error == ERROR_ACCESS_DENIED) {
				xasprintf(cause, "access to server %s denied "
				    "(it may belong to another user)", label);
			} else {
				msg = win32_ipc_strerror(error);
				xasprintf(cause, "error connecting to %s (%s)",
				    label, msg);
				free(msg);
			}
			goto fail;
		}

		if (ipc_check_pipe(pipe, label, cause) != 0) {
			CloseHandle(pipe);
			goto fail;
		}
		if (ipc_pipe_read_line(pipe, line, sizeof line,
		    deadline) != 0) {
			CloseHandle(pipe);
			xasprintf(cause, "server %s is not responding", label);
			goto fail;
		}
		CloseHandle(pipe);

		if (strcmp(line, "busy") != 0)
			break;
		if (GetTickCount64() >= deadline) {
			xasprintf(cause, "server %s is too busy", label);
			goto fail;
		}
		Sleep(5 + (GetCurrentProcessId() + GetTickCount()) % 20);
	}
	free(name);
	name = NULL;

	/* "<port>:<nonce>:<proof>"; servers before the proof omit it. */
	port = strtoul(line, &end, 10);
	if (end == line || *end != ':' || port == 0 || port > 65535)
		goto bad_reply;
	nonce = end + 1;
	if ((proof = strchr(nonce, ':')) != NULL)
		*proof++ = '\0';
	if (!ipc_valid_token(nonce) || (proof != NULL &&
	    !ipc_valid_token(proof)))
		goto bad_reply;

	if ((s = ipc_socket()) == INVALID_SOCKET)
		goto socket_failed;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons((u_short)port);
	if (connect(s, (struct sockaddr *)&addr, sizeof addr) != 0)
		goto socket_failed;

	if (tty_token != NULL)
		xasprintf(&auth, "TTY:%s:%s\n", tty_token, nonce);
	else
		xasprintf(&auth, "%s\n", nonce);
	sent = ipc_send_all(s, auth, strlen(auth));
	SecureZeroMemory(auth, strlen(auth));
	free(auth);
	if (sent != 0)
		goto socket_failed;

	if (proof != NULL) {
		deadline = GetTickCount64() + IPC_REPLY_TIMEOUT;
		if (ipc_recv_line(s, reply, sizeof reply, deadline) != 0 ||
		    !ipc_valid_token(reply) || !ipc_token_equal(reply, proof)) {
			xasprintf(cause, "server %s failed to authenticate; "
			    "refusing to connect", label);
			goto fail;
		}
	}
	SecureZeroMemory(line, sizeof line);
	*status = WIN32_IPC_OK;
	return ((int)s);

bad_reply:
	xasprintf(cause, "bad reply from server %s", label);
	goto fail;

socket_failed:
	msg = win32_ipc_strerror(WSAGetLastError());
	xasprintf(cause, "error connecting to %s (%s)", label, msg);
	free(msg);

fail:
	SecureZeroMemory(line, sizeof line);
	if (s != INVALID_SOCKET)
		closesocket(s);
	free(name);
	return (-1);
}

/*
 * Create a connected pair of non-inheritable loopback sockets, making sure
 * the accepted end really is our own connection.
 */
int
win32_ipc_socketpair(int sv[2])
{
	SOCKET			 listener, s1 = INVALID_SOCKET, s2;
	struct sockaddr_in	 addr, local, peer;
	int			 len, tries;

	win32_wsa_init();

	if ((listener = ipc_socket()) == INVALID_SOCKET)
		return (-1);
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	len = sizeof addr;
	if (bind(listener, (struct sockaddr *)&addr, sizeof addr) != 0 ||
	    getsockname(listener, (struct sockaddr *)&addr, &len) != 0 ||
	    listen(listener, 4) != 0)
		goto fail;

	if ((s1 = ipc_socket()) == INVALID_SOCKET)
		goto fail;
	if (connect(s1, (struct sockaddr *)&addr, sizeof addr) != 0)
		goto fail;
	len = sizeof local;
	if (getsockname(s1, (struct sockaddr *)&local, &len) != 0)
		goto fail;

	for (tries = 0; tries < 4; tries++) {
		len = sizeof peer;
		s2 = accept(listener, (struct sockaddr *)&peer, &len);
		if (s2 == INVALID_SOCKET)
			goto fail;
		if (peer.sin_port == local.sin_port &&
		    peer.sin_addr.s_addr == local.sin_addr.s_addr) {
			SetHandleInformation((HANDLE)s2, HANDLE_FLAG_INHERIT,
			    0);
			closesocket(listener);
			sv[0] = (int)s1;
			sv[1] = (int)s2;
			return (0);
		}
		/* Somebody else got in first. */
		closesocket(s2);
	}

fail:
	if (s1 != INVALID_SOCKET)
		closesocket(s1);
	closesocket(listener);
	return (-1);
}

/*
 * Serialize starting a server for a label across clients. Returns a handle
 * to pass to win32_server_unlock (NULL if locking was not possible, in which
 * case the caller carries on unlocked).
 */
HANDLE
win32_server_lock(const char *label)
{
	SECURITY_ATTRIBUTES	 sa;
	PSECURITY_DESCRIPTOR	 sd;
	wchar_t			*w, *name;
	size_t			 len;
	HANDLE			 h;
	DWORD			 r;

	if ((w = win32_ipc_utf8_to_wide(label)) == NULL)
		return (NULL);
	len = wcslen(w) + 32;
	name = xreallocarray(NULL, len, sizeof *name);
	_snwprintf_s(name, len, _TRUNCATE, L"Global\\%ls-start", w);
	free(w);

	sd = ipc_owner_only_sd();
	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = sd;
	sa.bInheritHandle = FALSE;
	h = CreateMutexW(&sa, FALSE, name);
	if (sd != NULL)
		LocalFree(sd);
	free(name);
	if (h == NULL) {
		log_debug("%s: CreateMutex failed: %lu", __func__,
		    (u_long)GetLastError());
		return (NULL);
	}

	r = WaitForSingleObject(h, IPC_LOCK_TIMEOUT);
	if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) {
		log_debug("%s: lock wait failed: %lu", __func__, (u_long)r);
		CloseHandle(h);
		return (NULL);
	}
	return (h);
}

/* Release the server start lock. */
void
win32_server_unlock(HANDLE h)
{
	if (h != NULL) {
		ReleaseMutex(h);
		CloseHandle(h);
	}
}

/* Append an argument to a command line, quoted for CommandLineToArgvW. */
static void
launch_append_arg(wchar_t **cmd, size_t *len, const wchar_t *arg)
{
	size_t		 need, n, slashes;
	const wchar_t	*p;
	wchar_t		*out;

	/* Worst case every character is escaped, plus quotes and space. */
	need = *len + wcslen(arg) * 2 + 4;
	*cmd = xreallocarray(*cmd, need, sizeof **cmd);
	out = *cmd + *len;
	if (*len != 0)
		*out++ = L' ';
	*out++ = L'"';
	for (p = arg;; p++) {
		slashes = 0;
		while (*p == L'\\') {
			slashes++;
			p++;
		}
		if (*p == L'\0') {
			/* Double trailing backslashes before the quote. */
			for (n = 0; n < slashes * 2; n++)
				*out++ = L'\\';
			break;
		}
		if (*p == L'"') {
			for (n = 0; n < slashes * 2 + 1; n++)
				*out++ = L'\\';
		} else {
			for (n = 0; n < slashes; n++)
				*out++ = L'\\';
		}
		*out++ = *p;
	}
	*out++ = L'"';
	*out = L'\0';
	*len = out - *cmd;
}

/*
 * Build an environment block of the current environment plus a marker so the
 * new process knows it was started automatically by a client.
 */
static wchar_t *
launch_environment(void)
{
	static const wchar_t	 marker[] = L"TMUX_WIN32_AUTOSTART=1";
	wchar_t			*env, *p, *block, *out;
	size_t			 len = 0, n;

	if ((env = GetEnvironmentStringsW()) == NULL)
		return (NULL);
	for (p = env; *p != L'\0'; p += wcslen(p) + 1)
		len += wcslen(p) + 1;

	block = xreallocarray(NULL, len + nitems(marker) + 1, sizeof *block);
	out = block;
	for (p = env; *p != L'\0'; p += n + 1) {
		n = wcslen(p);
		if (_wcsnicmp(p, L"TMUX_WIN32_AUTOSTART=", 21) == 0)
			continue;
		memcpy(out, p, (n + 1) * sizeof *out);
		out += n + 1;
	}
	memcpy(out, marker, sizeof marker);
	out += nitems(marker);
	*out = L'\0';

	FreeEnvironmentStringsW(env);
	return (block);
}

/*
 * Launch a detached tmux server process: tmux.exe <argv...>, where argv
 * includes -D. The server's standard error goes to errpath (if given) so a
 * failure to start can be reported by the client; standard input and output
 * go to NUL. Returns the process handle or NULL with *cause set.
 */
HANDLE
win32_launch_server(int argc, char **argv, const char *errpath, char **cause)
{
	STARTUPINFOEXW			 si;
	PROCESS_INFORMATION		 pi;
	SECURITY_ATTRIBUTES		 sa;
	LPPROC_THREAD_ATTRIBUTE_LIST	 attrs = NULL;
	SIZE_T				 attrs_size = 0;
	HANDLE				 handles[3], nul_in, nul_out, err;
	wchar_t				 exe[32768], *cmd = NULL, *arg, *env;
	wchar_t				*wpath;
	size_t				 len = 0;
	DWORD				 flags, error, n;
	char				*msg;
	int				 i, ok;

	*cause = NULL;
	memset(&pi, 0, sizeof pi);
	n = GetModuleFileNameW(NULL, exe, nitems(exe));
	if (n == 0 || n == nitems(exe)) {
		xasprintf(cause, "can't find tmux executable");
		return (NULL);
	}
	launch_append_arg(&cmd, &len, exe);
	for (i = 0; i < argc; i++) {
		if ((arg = win32_ipc_utf8_to_wide(argv[i])) == NULL) {
			xasprintf(cause, "bad argument: %s", argv[i]);
			free(cmd);
			return (NULL);
		}
		launch_append_arg(&cmd, &len, arg);
		free(arg);
	}

	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	sa.bInheritHandle = TRUE;
	nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ|
	    FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
	nul_out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ|
	    FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
	err = INVALID_HANDLE_VALUE;
	if (errpath != NULL &&
	    (wpath = win32_ipc_utf8_to_wide(errpath)) != NULL) {
		err = CreateFileW(wpath, GENERIC_WRITE, FILE_SHARE_READ|
		    FILE_SHARE_WRITE|FILE_SHARE_DELETE, &sa, CREATE_ALWAYS,
		    FILE_ATTRIBUTE_NORMAL, NULL);
		free(wpath);
	}
	if (err == INVALID_HANDLE_VALUE) {
		err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ|
		    FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
	}
	if (nul_in == INVALID_HANDLE_VALUE || nul_out == INVALID_HANDLE_VALUE ||
	    err == INVALID_HANDLE_VALUE) {
		msg = win32_ipc_strerror(GetLastError());
		xasprintf(cause, "can't open server output (%s)", msg);
		free(msg);
		goto out;
	}

	/* Only the three standard handles are inherited. */
	handles[0] = nul_in;
	handles[1] = nul_out;
	handles[2] = err;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attrs_size);
	attrs = xmalloc(attrs_size);
	if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrs_size)) {
		free(attrs);
		attrs = NULL;
	} else if (!UpdateProcThreadAttribute(attrs, 0,
	    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles, NULL,
	    NULL)) {
		DeleteProcThreadAttributeList(attrs);
		free(attrs);
		attrs = NULL;
	}

	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = nul_in;
	si.StartupInfo.hStdOutput = nul_out;
	si.StartupInfo.hStdError = err;
	si.lpAttributeList = attrs;
	memset(&pi, 0, sizeof pi);

	env = launch_environment();
	flags = DETACHED_PROCESS|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT;
	if (attrs != NULL)
		flags |= EXTENDED_STARTUPINFO_PRESENT;

	/*
	 * CREATE_BREAKAWAY_FROM_JOB detaches the server from any job object
	 * the parent belongs to (e.g., sshd's session job). Without this,
	 * closing an SSH connection kills the tmux server along with all
	 * processes in the job. Fall back without the flag if the job
	 * doesn't allow breakaway (ERROR_ACCESS_DENIED).
	 */
	ok = CreateProcessW(NULL, cmd, NULL, NULL, attrs != NULL,
	    flags|CREATE_BREAKAWAY_FROM_JOB, env, NULL, &si.StartupInfo, &pi);
	if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
		ok = CreateProcessW(NULL, cmd, NULL, NULL, attrs != NULL,
		    flags, env, NULL, &si.StartupInfo, &pi);
	}
	if (!ok) {
		error = GetLastError();
		msg = win32_ipc_strerror(error);
		xasprintf(cause, "can't start server (%s)", msg);
		free(msg);
	} else
		CloseHandle(pi.hThread);
	free(env);

out:
	if (attrs != NULL) {
		DeleteProcThreadAttributeList(attrs);
		free(attrs);
	}
	if (nul_in != INVALID_HANDLE_VALUE)
		CloseHandle(nul_in);
	if (nul_out != INVALID_HANDLE_VALUE)
		CloseHandle(nul_out);
	if (err != INVALID_HANDLE_VALUE)
		CloseHandle(err);
	free(cmd);
	if (*cause != NULL)
		return (NULL);
	return (pi.hProcess);
}
