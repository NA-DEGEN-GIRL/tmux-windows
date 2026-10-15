/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/file.h>
#endif

#include <errno.h>
#include <fcntl.h>
#ifndef _WIN32
#include <signal.h>
#endif
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "tmux.h"

static struct tmuxproc	*client_proc;
static struct tmuxpeer	*client_peer;
static uint64_t		 client_flags;
static int		 client_suspended;
#ifdef _WIN32
/*
 * The tty channel: a second connection to the server carrying the terminal
 * (or, in control mode, standard input and output). Output from the server is
 * written to the console by the event loop and console input is sent by a
 * thread, since libevent cannot wait on console handles.
 */
static int		 client_tty_fd = -1;
static char		 client_tty_token[65];
static struct event	 client_tty_event;
static HANDLE		 client_tty_out = INVALID_HANDLE_VALUE;
static HANDLE		 client_stdin_thread_h = NULL;
static volatile int	 client_tty_running = 0;
static int		 client_tty_relaying;
static int		 client_tty_raw;
static int		 client_tty_eof;

static HANDLE		 client_server_process;
static char		*client_server_errlog;
static char		*client_connect_cause;
#endif
static enum {
	CLIENT_EXIT_NONE,
	CLIENT_EXIT_DETACHED,
	CLIENT_EXIT_DETACHED_HUP,
	CLIENT_EXIT_LOST_TTY,
	CLIENT_EXIT_TERMINATED,
	CLIENT_EXIT_LOST_SERVER,
	CLIENT_EXIT_EXITED,
	CLIENT_EXIT_SERVER_EXITED,
	CLIENT_EXIT_MESSAGE_PROVIDED
} client_exitreason = CLIENT_EXIT_NONE;
static int		 client_exitflag;
static int		 client_exitval;
static enum msgtype	 client_exittype;
static const char	*client_exitsession;
static char		*client_exitmessage;
static const char	*client_execshell;
static const char	*client_execcmd;
static int		 client_attached;
static struct client_files client_files = RB_INITIALIZER(&client_files);

static __dead void	 client_exec(const char *,const char *);
static int		 client_get_lock(char *);
static int		 client_connect(struct event_base *, const char *,
			     uint64_t);
static void		 client_send_identify(const char *, const char *,
			     char **, u_int, const char *, int);
static void		 client_signal(int);
static void		 client_dispatch(struct imsg *, void *);
static void		 client_dispatch_attached(struct imsg *);
static void		 client_dispatch_wait(struct imsg *);
static const char	*client_exit_message(void);
#ifdef _WIN32
static int		 client_start_server(const char *, char **);
static char		*client_server_failure(void);
static void		 client_start_tty_relay(int);
static void		 client_stop_tty_relay(void);
static void		 client_tty_drain(DWORD);
static void		 client_start_stdin_thread(void);
static void		 client_stop_stdin_thread(void);
static void		 client_lock(const char *);
static void		 client_send_resize(void);
static void		 client_kill_parent(pid_t);
#endif

/*
 * Get server create lock. If already held then server start is happening in
 * another client, so block until the lock is released and return -2 to
 * retry. Return -1 on failure to continue and start the server anyway.
 */
static int
client_get_lock(char *lockfile)
{
	int lockfd;

	log_debug("lock file is %s", lockfile);

	if ((lockfd = open(lockfile, O_WRONLY|O_CREAT, 0600)) == -1) {
		log_debug("open failed: %s", strerror(errno));
		return (-1);
	}

	if (flock(lockfd, LOCK_EX|LOCK_NB) == -1) {
		log_debug("flock failed: %s", strerror(errno));
		if (errno != EAGAIN)
			return (lockfd);
		while (flock(lockfd, LOCK_EX) == -1 && errno == EINTR)
			/* nothing */;
		close(lockfd);
		return (-2);
	}
	log_debug("flock succeeded");

	return (lockfd);
}

/* Connect client to server. */
static int
client_connect(struct event_base *base, const char *path, uint64_t flags)
{
#ifdef _WIN32
	int	 fd, status;
	char	*cause = NULL;
	HANDLE	 lock;

	log_debug("connecting to server label %s", path);

	/*
	 * A server started by a client goes straight to starting. Anything
	 * else tries to connect first (so, as on Unix, an explicit -D becomes
	 * a client of a server that is already running).
	 */
	if (!win32_server_autostart) {
		fd = win32_ipc_connect(path, NULL, &status, &cause);
		if (fd != -1)
			goto connected;
		if (status != WIN32_IPC_NOSERVER)
			goto failed;
		log_debug("no server running");
	}

	if ((flags & CLIENT_NOSTARTSERVER) || (~flags & CLIENT_STARTSERVER)) {
		errno = ECONNREFUSED;
		return (-1);
	}

	if (flags & CLIENT_NOFORK) {
		/*
		 * This process IS the server (launched with -D). Call
		 * server_start() inline; it enters the event loop and never
		 * returns for CLIENT_NOFORK.
		 */
		return (server_start(client_proc, flags, base, -1, NULL));
	}

	/*
	 * Only one client starts the server for a label at a time; the others
	 * wait and then find it running. Start one only if there is definitely
	 * no server.
	 */
	lock = win32_server_lock(path);
	fd = win32_ipc_connect(path, NULL, &status, &cause);
	if (fd == -1 && status == WIN32_IPC_NOSERVER)
		fd = client_start_server(path, &cause);
	win32_server_unlock(lock);
	if (fd == -1)
		goto failed;

connected:
	setblocking(fd, 0);
	return (fd);

failed:
	client_connect_cause = cause;
	errno = EIO;
	return (-1);
#else
	struct sockaddr_un	sa;
	size_t			size;
	int			fd, lockfd = -1, locked = 0;
	char		       *lockfile = NULL;

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	size = strlcpy(sa.sun_path, path, sizeof sa.sun_path);
	if (size >= sizeof sa.sun_path) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	log_debug("socket is %s", path);

retry:
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return (-1);

	log_debug("trying connect");
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == -1) {
		log_debug("connect failed: %s", strerror(errno));
		if (errno != ECONNREFUSED && errno != ENOENT)
			goto failed;
		if (flags & CLIENT_NOSTARTSERVER)
			goto failed;
		if (~flags & CLIENT_STARTSERVER)
			goto failed;
		close(fd);

		if (!locked) {
			xasprintf(&lockfile, "%s.lock", path);
			if ((lockfd = client_get_lock(lockfile)) < 0) {
				log_debug("didn't get lock (%d)", lockfd);

				free(lockfile);
				lockfile = NULL;

				if (lockfd == -2)
					goto retry;
			}
			log_debug("got lock (%d)", lockfd);

			/*
			 * Always retry at least once, even if we got the lock,
			 * because another client could have taken the lock,
			 * started the server and released the lock between our
			 * connect() and flock().
			 */
			locked = 1;
			goto retry;
		}

		if (lockfd >= 0 && unlink(path) != 0 && errno != ENOENT) {
			free(lockfile);
			close(lockfd);
			return (-1);
		}
		fd = server_start(client_proc, flags, base, lockfd, lockfile);
	}

	if (locked && lockfd >= 0) {
		free(lockfile);
		close(lockfd);
	}
	setblocking(fd, 0);
	return (fd);

failed:
	if (locked) {
		free(lockfile);
		close(lockfd);
	}
	close(fd);
	return (-1);
#endif
}

/* Get exit string from reason number. */
const char *
client_exit_message(void)
{
	static char msg[256];

	switch (client_exitreason) {
	case CLIENT_EXIT_NONE:
		break;
	case CLIENT_EXIT_DETACHED:
		if (client_exitsession != NULL) {
			xsnprintf(msg, sizeof msg, "detached "
			    "(from session %s)", client_exitsession);
			return (msg);
		}
		return ("detached");
	case CLIENT_EXIT_DETACHED_HUP:
		if (client_exitsession != NULL) {
			xsnprintf(msg, sizeof msg, "detached and SIGHUP "
			    "(from session %s)", client_exitsession);
			return (msg);
		}
		return ("detached and SIGHUP");
	case CLIENT_EXIT_LOST_TTY:
		return ("lost tty");
	case CLIENT_EXIT_TERMINATED:
		return ("terminated");
	case CLIENT_EXIT_LOST_SERVER:
		return ("server exited unexpectedly");
	case CLIENT_EXIT_EXITED:
		return ("exited");
	case CLIENT_EXIT_SERVER_EXITED:
		return ("server exited");
	case CLIENT_EXIT_MESSAGE_PROVIDED:
		return (client_exitmessage);
	}
	return ("unknown reason");
}

/* Exit if all streams flushed. */
static void
client_exit(void)
{
	if (!file_write_left(&client_files))
		proc_exit(client_proc);
}

/* Client main loop. */
int
client_main(struct event_base *base, int argc, char **argv, uint64_t flags,
    int feat)
{
	struct cmd_parse_result	*pr;
	struct msg_command	*data;
	int			 fd, i;
	const char		*ttynam, *termname, *cwd;
	pid_t			 ppid;
	enum msgtype		 msg;
	struct termios		 tio, saved_tio;
	size_t			 size, linesize = 0;
	ssize_t			 linelen;
	char			*line = NULL, **caps = NULL, *cause;
	u_int			 ncaps = 0;
	struct args_value	*values;

	/* Set up the initial command. */
	if (shell_command != NULL) {
		msg = MSG_SHELL;
		flags |= CLIENT_STARTSERVER;
	} else if (argc == 0) {
		msg = MSG_COMMAND;
		flags |= CLIENT_STARTSERVER;
	} else {
		msg = MSG_COMMAND;

		/*
		 * It's annoying parsing the command string twice (in client
		 * and later in server) but it is necessary to get the start
		 * server flag.
		 */
		values = args_from_vector(argc, argv);
		pr = cmd_parse_from_arguments(values, argc, NULL);
		if (pr->status == CMD_PARSE_SUCCESS) {
			if (cmd_list_any_have(pr->cmdlist, CMD_STARTSERVER))
				flags |= CLIENT_STARTSERVER;
			cmd_list_free(pr->cmdlist);
		} else
			free(pr->error);
		args_free_values(values, argc);
		free(values);
	}

	/* Create client process structure (starts logging). */
	client_proc = proc_start("client");
	proc_set_signals(client_proc, client_signal);

	/* Save the flags. */
	client_flags = flags;
	log_debug("flags are %#llx", (unsigned long long)client_flags);

	/* Initialize the client socket and start the server. */
#ifdef HAVE_SYSTEMD
	if (systemd_activated()) {
		/* socket-based activation, do not even try to be a client. */
		fd = server_start(client_proc, flags, base, 0, NULL);
	} else
#endif
	fd = client_connect(base, socket_path, client_flags);
	if (fd == -1) {
#ifdef _WIN32
		if (client_connect_cause != NULL) {
			fprintf(stderr, "%s\n", client_connect_cause);
			return (1);
		}
#endif
		if (errno == ECONNREFUSED) {
			fprintf(stderr, "no server running on %s\n",
			    socket_path);
		} else {
			fprintf(stderr, "error connecting to %s (%s)\n",
			    socket_path, strerror(errno));
		}
		return (1);
	}
	client_peer = proc_add_peer(client_proc, fd, client_dispatch, NULL);

#ifdef _WIN32
	/*
	 * Open the tty channel if there is a terminal (a console on standard
	 * input, as on Unix the terminal is standard input) or for control
	 * mode, where it carries standard input and output. Clients run with
	 * pipes, by scripts and tools, have no use for it and do not wait for
	 * it. It is set up before identifying so the server can match it
	 * straight away.
	 */
	if ((client_flags & CLIENT_CONTROL) ||
	    win32_tty_is_console(GetStdHandle(STD_INPUT_HANDLE))) {
		int	 status;
		char	*tty_cause = NULL;

		win32_generate_tty_token(client_tty_token,
		    sizeof client_tty_token);
		if (*client_tty_token != '\0') {
			client_tty_fd = win32_ipc_connect(socket_path,
			    client_tty_token, &status, &tty_cause);
		}
		if (client_tty_fd == -1) {
			if (client_flags & CLIENT_CONTROL) {
				fprintf(stderr, "%s\n", tty_cause != NULL ?
				    tty_cause : "can't open control channel");
				return (1);
			}
			log_debug("no tty channel: %s", tty_cause != NULL ?
			    tty_cause : "no token");
		}
		free(tty_cause);
	}
#endif

	/* Save these before pledge(). */
	if ((cwd = find_cwd()) == NULL && (cwd = find_home()) == NULL)
#ifdef _WIN32
		cwd = "C:\\";
#else
		cwd = "/";
#endif
	if ((ttynam = ttyname(STDIN_FILENO)) == NULL)
		ttynam = "";
	if ((termname = getenv("TERM")) == NULL)
#ifdef _WIN32
		termname = "xterm-256color";
#else
		termname = "";
#endif

	/*
	 * Drop privileges for client. "proc exec" is needed for -c and for
	 * locking (which uses system(3)).
	 *
	 * "tty" is needed to restore termios(4) and also for some reason -CC
	 * does not work properly without it (input is not recognised).
	 *
	 * "sendfd" is dropped later in client_dispatch_wait().
	 */
	if (pledge(
	    "stdio rpath wpath cpath unix sendfd proc exec tty",
	    NULL) != 0)
		fatal("pledge failed");

	/* Load terminfo entry if any. */
	if (isatty(STDIN_FILENO) &&
	    *termname != '\0' &&
	    tty_term_read_list(termname, STDIN_FILENO, &caps, &ncaps,
	    &cause) != 0) {
		fprintf(stderr, "%s\n", cause);
		free(cause);
		return (1);
	}

	/* Free stuff that is not used in the client. */
	if (ptm_fd != -1)
		close(ptm_fd);
	options_free(global_options);
	options_free(global_s_options);
	options_free(global_w_options);
	environ_free(global_environ);

	/* Set up control mode. */
	if (client_flags & CLIENT_CONTROLCONTROL) {
		if (tcgetattr(STDIN_FILENO, &saved_tio) != 0) {
			fprintf(stderr, "tcgetattr failed: %s\n",
			    strerror(errno));
			return (1);
		}
		cfmakeraw(&tio);
		tio.c_iflag = ICRNL|IXANY;
		tio.c_oflag = OPOST|ONLCR;
#ifdef NOKERNINFO
		tio.c_lflag = NOKERNINFO;
#endif
		tio.c_cflag = CREAD|CS8|HUPCL;
		tio.c_cc[VMIN] = 1;
		tio.c_cc[VTIME] = 0;
		cfsetispeed(&tio, cfgetispeed(&saved_tio));
		cfsetospeed(&tio, cfgetospeed(&saved_tio));
		tcsetattr(STDIN_FILENO, TCSANOW, &tio);
	}

	/* Send identify messages. */
	client_send_identify(ttynam, termname, caps, ncaps, cwd, feat);
	tty_term_free_list(caps, ncaps);
	proc_flush_peer(client_peer);

	/* Send first command. */
	if (msg == MSG_COMMAND) {
		/* How big is the command? */
		size = 0;
		for (i = 0; i < argc; i++)
			size += strlen(argv[i]) + 1;
		if (size > MAX_IMSGSIZE - (sizeof *data)) {
			fprintf(stderr, "command too long\n");
			return (1);
		}
		data = xmalloc((sizeof *data) + size);

		/* Prepare command for server. */
		data->argc = argc;
		if (cmd_pack_argv(argc, argv, (char *)(data + 1), size) != 0) {
			fprintf(stderr, "command too long\n");
			free(data);
			return (1);
		}
		size += sizeof *data;

		/* Send the command. */
		if (proc_send(client_peer, msg, -1, data, size) != 0) {
			fprintf(stderr, "failed to send command\n");
			free(data);
			return (1);
		}
		free(data);
	} else if (msg == MSG_SHELL)
		proc_send(client_peer, msg, -1, NULL, 0);

#ifdef _WIN32
	/*
	 * Control mode clients are never sent MSG_READY: relay standard input
	 * and output through the channel from the start.
	 */
	if (client_flags & CLIENT_CONTROL)
		client_start_tty_relay(0);
#endif

	/* Start main loop. */
	proc_loop(client_proc, NULL);

#ifdef _WIN32
	/* Stop tty relay and restore console. */
	client_stop_tty_relay();

	/* If a server we started has died, say why if it left a reason. */
	if (client_exitreason == CLIENT_EXIT_LOST_SERVER &&
	    client_server_process != NULL &&
	    WaitForSingleObject(client_server_process, 1000) == WAIT_OBJECT_0) {
		client_exitmessage = client_server_failure();
		client_exitreason = CLIENT_EXIT_MESSAGE_PROVIDED;
	}
#endif

	/* Run command if user requested exec, instead of exiting. */
	if (client_exittype == MSG_EXEC) {
		if (client_flags & CLIENT_CONTROLCONTROL)
			tcsetattr(STDOUT_FILENO, TCSAFLUSH, &saved_tio);
		client_exec(client_execshell, client_execcmd);
	}

	/* Restore streams to blocking. */
	setblocking(STDIN_FILENO, 1);
	setblocking(STDOUT_FILENO, 1);
	setblocking(STDERR_FILENO, 1);

	/* Print the exit message, if any, and exit. */
	if (client_attached) {
		if (client_exitreason != CLIENT_EXIT_NONE)
			printf("[%s]\n", client_exit_message());

		ppid = getppid();
		if (client_exittype == MSG_DETACHKILL && ppid > 1)
#ifdef _WIN32
			client_kill_parent(ppid);
#else
			kill(ppid, SIGHUP);
#endif
	} else if (client_flags & CLIENT_CONTROL) {
		if (client_exitreason != CLIENT_EXIT_NONE)
			printf("%%exit %s\n", client_exit_message());
		else
			printf("%%exit\n");
		fflush(stdout);
		if (client_flags & CLIENT_CONTROL_WAITEXIT) {
#ifdef _WIN32
			setvbuf(stdin, NULL, _IOLBF, BUFSIZ);
#else
			setvbuf(stdin, NULL, _IOLBF, 0);
#endif
			for (;;) {
				linelen = getline(&line, &linesize, stdin);
				if (linelen <= 1)
					break;
			}
			free(line);
		}
		if (client_flags & CLIENT_CONTROLCONTROL) {
			printf("\033\\");
			fflush(stdout);
			tcsetattr(STDOUT_FILENO, TCSAFLUSH, &saved_tio);
		}
	} else if (client_exitreason != CLIENT_EXIT_NONE)
		fprintf(stderr, "%s\n", client_exit_message());
	return (client_exitval);
}

/* Send identify messages to server. */
static void
client_send_identify(const char *ttynam, const char *termname, char **caps,
    u_int ncaps, const char *cwd, int feat)
{
	char	**ss;
	size_t	  sslen;
#ifndef _WIN32
	int	  fd;
#endif
	uint64_t  flags = client_flags;
	pid_t	  pid;
	u_int	  i;

	proc_send(client_peer, MSG_IDENTIFY_LONGFLAGS, -1, &flags, sizeof flags);
	proc_send(client_peer, MSG_IDENTIFY_LONGFLAGS, -1, &client_flags,
	    sizeof client_flags);

	proc_send(client_peer, MSG_IDENTIFY_TERM, -1, termname,
	    strlen(termname) + 1);
	proc_send(client_peer, MSG_IDENTIFY_FEATURES, -1, &feat, sizeof feat);

	proc_send(client_peer, MSG_IDENTIFY_TTYNAME, -1, ttynam,
	    strlen(ttynam) + 1);
	proc_send(client_peer, MSG_IDENTIFY_CWD, -1, cwd, strlen(cwd) + 1);

	for (i = 0; i < ncaps; i++) {
		proc_send(client_peer, MSG_IDENTIFY_TERMINFO, -1,
		    caps[i], strlen(caps[i]) + 1);
	}

#ifdef _WIN32
	proc_send(client_peer, MSG_IDENTIFY_STDIN, -1, NULL, 0);
	proc_send(client_peer, MSG_IDENTIFY_STDOUT, -1, NULL, 0);
	if (client_tty_fd != -1) {
		proc_send(client_peer, MSG_IDENTIFY_TTYTOKEN, -1,
		    client_tty_token, strlen(client_tty_token) + 1);
	}
#else
	if ((fd = dup(STDIN_FILENO)) == -1)
		fatal("dup failed");
	proc_send(client_peer, MSG_IDENTIFY_STDIN, fd, NULL, 0);
	if ((fd = dup(STDOUT_FILENO)) == -1)
		fatal("dup failed");
	proc_send(client_peer, MSG_IDENTIFY_STDOUT, fd, NULL, 0);
#endif

	pid = getpid();
	proc_send(client_peer, MSG_IDENTIFY_CLIENTPID, -1, &pid, sizeof pid);

	for (ss = environ; *ss != NULL; ss++) {
		sslen = strlen(*ss) + 1;
		if (sslen > MAX_IMSGSIZE - IMSG_HEADER_SIZE)
			continue;
		proc_send(client_peer, MSG_IDENTIFY_ENVIRON, -1, *ss, sslen);
	}

	proc_send(client_peer, MSG_IDENTIFY_DONE, -1, NULL, 0);

#ifdef _WIN32
	/*
	 * Send initial window size from the client's console. The server
	 * is a detached process with no console, so it cannot detect the
	 * client's terminal dimensions on its own.
	 */
	if (client_tty_fd != -1 && (~client_flags & CLIENT_CONTROL))
		client_send_resize();
#endif
}

/* Run command in shell; used for -c. */
static __dead void
client_exec(const char *shell, const char *shellcmd)
{
#ifdef _WIN32
	char cmd[32768];

	log_debug("shell %s, command %s", shell, shellcmd);
	proc_clear_signals(client_proc, 1);

	snprintf(cmd, sizeof cmd, "\"%s\" /c %s", shell, shellcmd);
	system(cmd);
	exit(0);
#else
	char	*argv0;

	log_debug("shell %s, command %s", shell, shellcmd);
	argv0 = shell_argv0(shell, !!(client_flags & CLIENT_LOGIN));
	setenv("SHELL", shell, 1);

	proc_clear_signals(client_proc, 1);

	setblocking(STDIN_FILENO, 1);
	setblocking(STDOUT_FILENO, 1);
	setblocking(STDERR_FILENO, 1);
	closefrom(STDERR_FILENO + 1);

	execl(shell, argv0, "-c", shellcmd, (char *) NULL);
	fatal("execl failed");
#endif
}

#ifdef _WIN32
/* Get the file where a server's standard error goes, creating its directory. */
static char *
client_server_log_path(const char *label)
{
	wchar_t	*appdata, *wdir;
	char	*base, *dir, *path;

	if ((appdata = _wgetenv(L"LOCALAPPDATA")) == NULL &&
	    (appdata = _wgetenv(L"TEMP")) == NULL)
		return (NULL);
	if ((base = win32_ipc_wide_to_utf8(appdata)) == NULL)
		return (NULL);
	xasprintf(&dir, "%s\\tmux", base);
	free(base);
	if ((wdir = win32_ipc_utf8_to_wide(dir)) != NULL) {
		_wmkdir(wdir);
		free(wdir);
	}
	xasprintf(&path, "%s\\%s.server.log", dir, label);
	free(dir);
	return (path);
}

/* Explain why the server we started has exited, from what it left behind. */
static char *
client_server_failure(void)
{
	wchar_t	*wpath;
	HANDLE	 h;
	char	 buf[4096], *msg, *cp;
	DWORD	 n = 0, code = 0;

	*buf = '\0';
	if (client_server_errlog != NULL &&
	    (wpath = win32_ipc_utf8_to_wide(client_server_errlog)) != NULL) {
		h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ|
		    FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0,
		    NULL);
		free(wpath);
		if (h != INVALID_HANDLE_VALUE) {
			if (!ReadFile(h, buf, sizeof buf - 1, &n, NULL))
				n = 0;
			buf[n] = '\0';
			CloseHandle(h);
		}
	}
	cp = buf + strlen(buf);
	while (cp > buf && (cp[-1] == '\r' || cp[-1] == '\n' || cp[-1] == ' '))
		*--cp = '\0';

	if (client_server_process != NULL)
		GetExitCodeProcess(client_server_process, &code);
	if (*buf != '\0')
		xasprintf(&msg, "server exited unexpectedly: %s", buf);
	else {
		xasprintf(&msg, "server exited unexpectedly (exit code %lu)",
		    (u_long)code);
	}
	return (msg);
}

/* Make a configuration file path absolute for a new server. */
static char *
client_server_config_path(const char *path)
{
	char	*copy, resolved[PATH_MAX];

	if (_stricmp(path, "NUL") == 0 || strncmp(path, "~/", 2) == 0)
		return (xstrdup(path));
	copy = xstrdup(path);
	/* Undo the MSYS escape of a drive colon: C\:/foo is C:/foo. */
	if (copy[0] != '\0' && copy[1] == '\\' && copy[2] == ':')
		memmove(copy + 1, copy + 2, strlen(copy + 2) + 1);
	if (_fullpath(resolved, copy, sizeof resolved) == NULL)
		return (copy);
	free(copy);
	return (xstrdup(resolved));
}

/*
 * Start a server process and connect to it. The server is given the same
 * label, configuration files and verbosity that a forked server would have
 * inherited on Unix. Returns the connected socket or -1 with *cause set.
 */
static int
client_start_server(const char *path, char **cause)
{
	char		**argv = NULL;
	int		  argc = 0, fd = -1, status, i, level;
	u_int		  n;
	HANDLE		  process;
	ULONGLONG	  deadline;

	argv = xreallocarray(argv, 3 + 2 * cfg_nfiles + 8, sizeof *argv);
	argv[argc++] = xstrdup("-D");
	argv[argc++] = xstrdup("-S");
	argv[argc++] = xstrdup(path);
	if (!cfg_quiet) {
		/* Only when -f was given; otherwise the server finds them. */
		for (n = 0; n < cfg_nfiles; n++) {
			argv[argc++] = xstrdup("-f");
			argv[argc++] = client_server_config_path(cfg_files[n]);
		}
	}
	level = log_get_level();
	for (i = 0; i < level && i < 8; i++)
		argv[argc++] = xstrdup("-v");

	client_server_errlog = client_server_log_path(path);
	process = win32_launch_server(argc, argv, client_server_errlog, cause);
	for (i = 0; i < argc; i++)
		free(argv[i]);
	free(argv);
	if (process == NULL)
		return (-1);
	log_debug("started server process %lu", (u_long)GetProcessId(process));

	/* Wait for the server to appear, or to die trying. */
	deadline = GetTickCount64() + 10000;
	for (;;) {
		fd = win32_ipc_connect(path, NULL, &status, cause);
		if (fd != -1 || status != WIN32_IPC_NOSERVER)
			break;
		if (WaitForSingleObject(process, 10) == WAIT_OBJECT_0) {
			/* Another server may have won a race with ours. */
			fd = win32_ipc_connect(path, NULL, &status, cause);
			if (fd == -1 && status == WIN32_IPC_NOSERVER) {
				client_server_process = process;
				*cause = client_server_failure();
				client_server_process = NULL;
			}
			break;
		}
		if (GetTickCount64() > deadline) {
			xasprintf(cause, "server %s did not start", path);
			break;
		}
	}
	if (fd == -1) {
		CloseHandle(process);
		return (-1);
	}
	client_server_process = process;
	return (fd);
}

/* Write all of a buffer to where tty output goes. */
static void
client_tty_write(const char *buf, size_t len)
{
	DWORD	written;

	while (len > 0) {
		if (!WriteFile(client_tty_out, buf, (DWORD)len, &written,
		    NULL) || written == 0)
			return;
		buf += written;
		len -= written;
	}
}

/* Tty relay: copy output from the server to the console (or stdout). */
static void
client_tty_read_cb(int fd, __unused short events, __unused void *arg)
{
	char	buf[65536];
	int	n, i;

	for (i = 0; i < 16; i++) {
		n = recv((SOCKET)fd, buf, sizeof buf, 0);
		if (n > 0) {
			client_tty_write(buf, n);
			continue;
		}
		if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
			return;
		/* The server has finished with the channel. */
		event_del(&client_tty_event);
		client_tty_eof = 1;
		return;
	}
}

/*
 * Copy whatever the server has still to send on the channel, until it ends
 * the channel or nothing arrives for a while.
 */
static void
client_tty_drain(DWORD idle)
{
	char		 buf[65536];
	fd_set		 set;
	struct timeval	 tv;
	ULONGLONG	 deadline;
	int		 n;

	if (client_tty_fd == -1 || client_tty_eof)
		return;
	deadline = GetTickCount64() + 3000;
	while (GetTickCount64() < deadline) {
		FD_ZERO(&set);
		FD_SET((SOCKET)client_tty_fd, &set);
		tv.tv_sec = idle / 1000;
		tv.tv_usec = (idle % 1000) * 1000;
		if (select(0, &set, NULL, NULL, &tv) != 1)
			return;
		n = recv((SOCKET)client_tty_fd, buf, sizeof buf, 0);
		if (n > 0) {
			client_tty_write(buf, n);
			continue;
		}
		if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
			continue;
		client_tty_eof = 1;
		return;
	}
}

/* Send all of a buffer on the non-blocking channel socket. */
static int
client_tty_send(SOCKET s, const char *buf, size_t len)
{
	fd_set		set;
	struct timeval	tv;
	int		n;

	while (len > 0) {
		n = send(s, buf, (int)len, 0);
		if (n > 0) {
			buf += n;
			len -= n;
			continue;
		}
		if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
			return (-1);

		/* The server is busy (a large paste): wait for room. */
		if (!client_tty_running)
			return (-1);
		FD_ZERO(&set);
		FD_SET(s, &set);
		tv.tv_sec = 0;
		tv.tv_usec = 100000;
		select(0, NULL, &set, NULL, &tv);
	}
	return (0);
}

/*
 * Tty relay: read from console stdin (or, in control mode, any stdin) and
 * send it to the server. Runs in a separate thread since reads block.
 */
static DWORD WINAPI
client_stdin_thread_func(LPVOID arg)
{
	SOCKET	 s = (SOCKET)(intptr_t)arg;
	HANDLE	 h = GetStdHandle(STD_INPUT_HANDLE);
	char	 buf[8192], *in, *out;
	DWORD	 nread, mode, error;
	int	 console, control, eof = 0;

	console = GetConsoleMode(h, &mode);
	control = (client_flags & CLIENT_CONTROL) != 0;

	while (client_tty_running) {
		if (!ReadFile(h, buf, sizeof buf, &nread, NULL)) {
			error = GetLastError();
			if (error == ERROR_OPERATION_ABORTED &&
			    client_tty_running && console)
				continue; /* Ctrl-C in a cooked console */
			eof = (error == ERROR_BROKEN_PIPE ||
			    error == ERROR_HANDLE_EOF);
			break;
		}
		if (!client_tty_running)
			break;
		if (nread == 0) {
			if (console && !control)
				continue;
			eof = 1; /* Ctrl-Z in a console or end of a pipe */
			break;
		}

		/* Control mode reads lines: a console ends them with CRLF. */
		if (control && console) {
			for (in = out = buf; in < buf + nread; in++) {
				if (in[0] == '\r' && in + 1 < buf + nread &&
				    in[1] == '\n')
					continue;
				*out++ = *in;
			}
			nread = (DWORD)(out - buf);
		}
		if (client_tty_send(s, buf, nread) != 0)
			break;
	}

	/* No more input: tell the server, as closing stdin does on Unix. */
	if (eof && control)
		shutdown(s, SD_SEND);
	return (0);
}

/* Start the thread reading stdin. */
static void
client_start_stdin_thread(void)
{
	if (client_tty_fd == -1 || client_stdin_thread_h != NULL)
		return;
	client_tty_running = 1;
	client_stdin_thread_h = CreateThread(NULL, 0, client_stdin_thread_func,
	    (LPVOID)(intptr_t)client_tty_fd, 0, NULL);
}

/* Stop the thread reading stdin, interrupting a blocked read. */
static void
client_stop_stdin_thread(void)
{
	int	i;

	if (client_stdin_thread_h == NULL)
		return;
	client_tty_running = 0;
	for (i = 0; i < 20; i++) {
		CancelSynchronousIo(client_stdin_thread_h);
		if (WaitForSingleObject(client_stdin_thread_h, 50) ==
		    WAIT_OBJECT_0)
			break;
	}
	CloseHandle(client_stdin_thread_h);
	client_stdin_thread_h = NULL;
}

/*
 * Start relaying the tty channel: as the terminal once attached (console in
 * raw mode), or as standard input and output in control mode.
 */
static void
client_start_tty_relay(int terminal)
{
	if (client_tty_fd == -1 || client_tty_relaying)
		return;
	client_tty_relaying = 1;

	if (terminal) {
		client_tty_out = win32_tty_output();
		client_tty_raw = (win32_tty_raw_mode() == 0);
	} else
		client_tty_out = GetStdHandle(STD_OUTPUT_HANDLE);

	/* Set tty socket to non-blocking for libevent. */
	setblocking(client_tty_fd, 0);

	/* Register tty socket for reading. */
	event_set(&client_tty_event, client_tty_fd,
	    EV_READ|EV_PERSIST, client_tty_read_cb, NULL);
	event_add(&client_tty_event, NULL);

	client_start_stdin_thread();
}

/*
 * Stop the tty relay on detach/exit: stop reading input, write out what the
 * server sent last (such as resetting the terminal) and restore the console.
 */
static void
client_stop_tty_relay(void)
{
	client_stop_stdin_thread();

	if (event_initialized(&client_tty_event))
		event_del(&client_tty_event);

	if (client_tty_fd != -1) {
		if (client_tty_relaying)
			client_tty_drain(1000);
		closesocket((SOCKET)client_tty_fd);
		client_tty_fd = -1;
	}

	/* Restore console mode. */
	if (client_tty_raw) {
		win32_tty_restore();
		client_tty_raw = 0;
	}
	client_tty_relaying = 0;
}

/*
 * Lock: hand the console to the lock command and take it back afterwards.
 * The server has already sent the sequences to clear the screen.
 */
static void
client_lock(const char *cmd)
{
	client_tty_drain(100);
	client_stop_stdin_thread();
	if (client_tty_raw)
		win32_tty_restore();

	if (system(cmd) == -1)
		log_debug("lock command failed: %s", strerror(errno));

	if (client_tty_raw)
		client_tty_raw = (win32_tty_raw_mode() == 0);
	client_start_stdin_thread();
}

/*
 * Tell the server the console size: it has no console of its own to ask, so
 * the client sends its dimensions with MSG_RESIZE.
 */
static void
client_send_resize(void)
{
	struct msg_resize_data	rd;
	int			cols = 80, rows = 24;

	win32_tty_get_size(&cols, &rows);
	rd.sx = cols;
	rd.sy = rows;
	proc_send(client_peer, MSG_RESIZE, -1, &rd, sizeof rd);
}

/*
 * Like SIGHUP to the parent on Unix (detach-client -P): end the process that
 * started the client, normally the shell. The process is checked to still be
 * our parent (created before us) so a reused process ID is never hit.
 */
static void
client_kill_parent(pid_t ppid)
{
	HANDLE		h;
	FILETIME	pc, sc, x1, x2, x3;

	h = OpenProcess(PROCESS_TERMINATE|PROCESS_QUERY_LIMITED_INFORMATION,
	    FALSE, (DWORD)ppid);
	if (h == NULL)
		return;
	if (GetProcessTimes(h, &pc, &x1, &x2, &x3) &&
	    GetProcessTimes(GetCurrentProcess(), &sc, &x1, &x2, &x3) &&
	    CompareFileTime(&pc, &sc) <= 0) {
		log_debug("terminating parent %ld", (long)ppid);
		TerminateProcess(h, 0);
	}
	CloseHandle(h);
}
#endif

/* Callback to handle signals in the client. */
static void
client_signal(int sig)
{
#ifndef _WIN32
	struct sigaction sigact;
	int		 status;
	pid_t		 pid;
#endif

	log_debug("%s: %s", __func__, strsignal(sig));
	if (sig == SIGCHLD) {
#ifndef _WIN32
		for (;;) {
			pid = waitpid(WAIT_ANY, &status, WNOHANG);
			if (pid == 0)
				break;
			if (pid == -1) {
				if (errno == ECHILD)
					break;
				log_debug("waitpid failed: %s",
				    strerror(errno));
			}
		}
#endif
	} else if (!client_attached) {
		if (sig == SIGTERM || sig == SIGHUP)
			proc_exit(client_proc);
	} else {
		switch (sig) {
		case SIGHUP:
			client_exitreason = CLIENT_EXIT_LOST_TTY;
			client_exitval = 1;
			proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
			break;
		case SIGTERM:
			if (!client_suspended)
				client_exitreason = CLIENT_EXIT_TERMINATED;
			client_exitval = 1;
			proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
			break;
		case SIGWINCH:
#ifdef _WIN32
			client_send_resize();
#else
			proc_send(client_peer, MSG_RESIZE, -1, NULL, 0);
#endif
			break;
#ifndef _WIN32
		case SIGCONT:
			memset(&sigact, 0, sizeof sigact);
			sigemptyset(&sigact.sa_mask);
			sigact.sa_flags = SA_RESTART;
			sigact.sa_handler = SIG_IGN;
			if (sigaction(SIGTSTP, &sigact, NULL) != 0)
				fatal("sigaction failed");
			proc_send(client_peer, MSG_WAKEUP, -1, NULL, 0);
			client_suspended = 0;
			break;
#endif
		}
	}
}

/* Callback for file write error or close. */
static void
client_file_check_cb(__unused struct client *c, __unused const char *path,
    __unused int error, __unused int closed, __unused struct evbuffer *buffer,
    __unused void *data)
{
	if (client_exitflag)
		client_exit();
}

/* Callback for client read events. */
static void
client_dispatch(struct imsg *imsg, __unused void *arg)
{
	if (imsg == NULL) {
		if (!client_exitflag) {
			client_exitreason = CLIENT_EXIT_LOST_SERVER;
			client_exitval = 1;
		}
		proc_exit(client_proc);
		return;
	}

	if (client_attached)
		client_dispatch_attached(imsg);
	else
		client_dispatch_wait(imsg);
}

/* Process an exit message. */
static void
client_dispatch_exit_message(char *data, size_t datalen)
{
	int	retval;

	if (datalen < sizeof retval && datalen != 0)
		fatalx("bad MSG_EXIT size");

	if (datalen >= sizeof retval) {
		memcpy(&retval, data, sizeof retval);
		client_exitval = retval;
	}

	if (datalen > sizeof retval) {
		datalen -= sizeof retval;
		data += sizeof retval;

		client_exitmessage = xmalloc(datalen);
		memcpy(client_exitmessage, data, datalen);
		client_exitmessage[datalen - 1] = '\0';

		client_exitreason = CLIENT_EXIT_MESSAGE_PROVIDED;
	}
}

/* Dispatch imsgs when in wait state (before MSG_READY). */
static void
client_dispatch_wait(struct imsg *imsg)
{
	char		*data;
	ssize_t		 datalen;
	static int	 pledge_applied;

	/*
	 * "sendfd" is no longer required once all of the identify messages
	 * have been sent. We know the server won't send us anything until that
	 * point (because we don't ask it to), so we can drop "sendfd" once we
	 * get the first message from the server.
	 */
	if (!pledge_applied) {
		if (pledge(
		    "stdio rpath wpath cpath unix proc exec tty",
		    NULL) != 0)
			fatal("pledge failed");
		pledge_applied = 1;
	}

	data = imsg->data;
	datalen = imsg->hdr.len - IMSG_HEADER_SIZE;

	switch (imsg->hdr.type) {
	case MSG_EXIT:
	case MSG_SHUTDOWN:
		client_dispatch_exit_message(data, datalen);
		client_exitflag = 1;
		client_exit();
		break;
	case MSG_READY:
		if (datalen != 0)
			fatalx("bad MSG_READY size");

		client_attached = 1;
#ifdef _WIN32
		client_start_tty_relay(1);
		client_send_resize();
#else
		proc_send(client_peer, MSG_RESIZE, -1, NULL, 0);
#endif
		break;
	case MSG_VERSION:
		if (datalen != 0)
			fatalx("bad MSG_VERSION size");

		fprintf(stderr, "protocol version mismatch "
		    "(client %d, server %u)\n", PROTOCOL_VERSION,
		    imsg->hdr.peerid & 0xff);
		client_exitval = 1;
		proc_exit(client_proc);
		break;
	case MSG_FLAGS:
		if (datalen != sizeof client_flags)
			fatalx("bad MSG_FLAGS string");

		memcpy(&client_flags, data, sizeof client_flags);
		log_debug("new flags are %#llx",
		    (unsigned long long)client_flags);
		break;
	case MSG_SHELL:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_SHELL string");

		client_exec(data, shell_command);
		/* NOTREACHED */
	case MSG_DETACH:
	case MSG_DETACHKILL:
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXITED:
		proc_exit(client_proc);
		break;
	case MSG_READ_OPEN:
		file_read_open(&client_files, client_peer, imsg, 1,
		    !(client_flags & CLIENT_CONTROL), client_file_check_cb,
		    NULL);
		break;
	case MSG_READ_CANCEL:
		file_read_cancel(&client_files, imsg);
		break;
	case MSG_WRITE_OPEN:
		file_write_open(&client_files, client_peer, imsg, 1,
		    !(client_flags & CLIENT_CONTROL), client_file_check_cb,
		    NULL);
		break;
	case MSG_WRITE:
		file_write_data(&client_files, imsg);
		break;
	case MSG_WRITE_CLOSE:
		file_write_close(&client_files, imsg);
		break;
	case MSG_OLDSTDERR:
	case MSG_OLDSTDIN:
	case MSG_OLDSTDOUT:
		fprintf(stderr, "server version is too old for client\n");
		proc_exit(client_proc);
		break;
	}
}

/* Dispatch imsgs in attached state (after MSG_READY). */
static void
client_dispatch_attached(struct imsg *imsg)
{
#ifndef _WIN32
	struct sigaction	 sigact;
#endif
	char			*data;
	ssize_t			 datalen;

	data = imsg->data;
	datalen = imsg->hdr.len - IMSG_HEADER_SIZE;

	switch (imsg->hdr.type) {
	case MSG_FLAGS:
		if (datalen != sizeof client_flags)
			fatalx("bad MSG_FLAGS string");

		memcpy(&client_flags, data, sizeof client_flags);
		log_debug("new flags are %#llx",
		    (unsigned long long)client_flags);
		break;
	case MSG_DETACH:
	case MSG_DETACHKILL:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_DETACH string");

		client_exitsession = xstrdup(data);
		client_exittype = imsg->hdr.type;
		if (imsg->hdr.type == MSG_DETACHKILL)
			client_exitreason = CLIENT_EXIT_DETACHED_HUP;
		else
			client_exitreason = CLIENT_EXIT_DETACHED;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXEC:
		if (datalen == 0 || data[datalen - 1] != '\0' ||
		    strlen(data) + 1 == (size_t)datalen)
			fatalx("bad MSG_EXEC string");
		client_execcmd = xstrdup(data);
		client_execshell = xstrdup(data + strlen(data) + 1);

		client_exittype = imsg->hdr.type;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXIT:
		client_dispatch_exit_message(data, datalen);
		if (client_exitreason == CLIENT_EXIT_NONE)
			client_exitreason = CLIENT_EXIT_EXITED;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXITED:
		if (datalen != 0)
			fatalx("bad MSG_EXITED size");

		proc_exit(client_proc);
		break;
	case MSG_SHUTDOWN:
		if (datalen != 0)
			fatalx("bad MSG_SHUTDOWN size");

		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		client_exitreason = CLIENT_EXIT_SERVER_EXITED;
		client_exitval = 1;
		break;
	case MSG_SUSPEND:
		if (datalen != 0)
			fatalx("bad MSG_SUSPEND size");

#ifdef _WIN32
		/*
		 * There is no job control to stop the client and nothing to
		 * continue it, so leaving it suspended would freeze it: wake
		 * up again at once instead (suspend-client just redraws).
		 */
		proc_send(client_peer, MSG_WAKEUP, -1, NULL, 0);
#else
		memset(&sigact, 0, sizeof sigact);
		sigemptyset(&sigact.sa_mask);
		sigact.sa_flags = SA_RESTART;
		sigact.sa_handler = SIG_DFL;
		if (sigaction(SIGTSTP, &sigact, NULL) != 0)
			fatal("sigaction failed");
		client_suspended = 1;
		kill(getpid(), SIGTSTP);
#endif
		break;
	case MSG_LOCK:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_LOCK string");

#ifdef _WIN32
		client_lock(data);
#else
		system(data);
#endif
		proc_send(client_peer, MSG_UNLOCK, -1, NULL, 0);
		break;
	}
}
