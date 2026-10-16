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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#endif

#include <errno.h>
#include <fcntl.h>
#ifndef _WIN32
#include <signal.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <termios.h>
#endif
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "tmux.h"

/*
 * Main server functions.
 */

struct clients		 clients;

struct tmuxproc		*server_proc;
static int		 server_fd = -1;
static uint64_t		 server_client_flags;
static int		 server_exit;
static struct event	 server_ev_accept;
static struct event	 server_ev_tidy;

#ifdef _WIN32
/*
 * Pending tty channel connections waiting to be matched with a client.
 * When a TTY channel connection arrives in server_accept(), we store it
 * here until the client sends MSG_IDENTIFY_TTYTOKEN to claim it.
 */
#define MAX_PENDING_TTYS    16
#define PENDING_TTY_EXPIRY  30  /* seconds */

struct pending_tty {
	char			 token[65];
	int			 fd;
	time_t			 created;
	TAILQ_ENTRY(pending_tty) entry;
};
static TAILQ_HEAD(, pending_tty) pending_ttys =
    TAILQ_HEAD_INITIALIZER(pending_ttys);

/*
 * Connections that have been accepted but have not yet sent their
 * authentication line. They are read without blocking so a peer that connects
 * and sends nothing cannot stall the server; there are only so many and each
 * has a deadline.
 */
#define MAX_PENDING_AUTHS	64
#define PENDING_AUTH_TIMEOUT	5 /* seconds */

struct pending_auth {
	int				 fd;
	struct event			 event;
	uint64_t			 deadline;
	struct win32_ipc_auth		 auth;
	TAILQ_ENTRY(pending_auth)	 entry;
};
static TAILQ_HEAD(, pending_auth) pending_auths =
    TAILQ_HEAD_INITIALIZER(pending_auths);
static u_int			  npending_auths;

/* How long a server started by a client waits for that client. */
#define SERVER_FIRST_CLIENT 10 /* seconds */
static uint64_t			  server_first_deadline;
static struct event		  server_ev_first;

static void	server_accept_authenticated(int, int, const char *);

/* The first client has had time to connect; the loop checks if empty. */
static void
server_first_event(__unused int fd, __unused short events,
    __unused void *data)
{
	log_debug("first client deadline");
	server_first_deadline = 0;
}

void
server_add_pending_tty(const char *token, int fd)
{
	struct pending_tty	*pt, *pt_next;
	time_t			 now;
	u_int			 count = 0;

	now = time(NULL);

	/* Expire stale entries. */
	TAILQ_FOREACH_SAFE(pt, &pending_ttys, entry, pt_next) {
		if (now - pt->created >= PENDING_TTY_EXPIRY) {
			log_debug("expiring pending tty: token=%s fd=%d",
			    pt->token, pt->fd);
			closesocket((SOCKET)pt->fd);
			TAILQ_REMOVE(&pending_ttys, pt, entry);
			free(pt);
		} else
			count++;
	}

	/* Enforce cap. */
	if (count >= MAX_PENDING_TTYS) {
		log_debug("pending tty queue full, rejecting fd=%d", fd);
		closesocket((SOCKET)fd);
		return;
	}

	pt = xcalloc(1, sizeof *pt);
	strlcpy(pt->token, token, sizeof pt->token);
	pt->fd = fd;
	pt->created = now;
	TAILQ_INSERT_TAIL(&pending_ttys, pt, entry);
	log_debug("added pending tty: token=%s fd=%d", token, fd);
}

int
server_get_pending_tty(const char *token)
{
	struct pending_tty	*pt, *pt_next;
	int			 fd;

	TAILQ_FOREACH_SAFE(pt, &pending_ttys, entry, pt_next) {
		if (strcmp(pt->token, token) == 0) {
			fd = pt->fd;
			TAILQ_REMOVE(&pending_ttys, pt, entry);
			free(pt);
			log_debug("matched pending tty: token=%s fd=%d",
			    token, fd);
			return (fd);
		}
	}
	return (-1);
}

/* Forget a pending authentication, closing its socket if asked. */
static void
server_free_pending_auth(struct pending_auth *pa, int close_fd)
{
	event_del(&pa->event);
	TAILQ_REMOVE(&pending_auths, pa, entry);
	npending_auths--;
	if (close_fd)
		closesocket((SOCKET)pa->fd);
	free(pa);
}

/* Data (or the deadline) for a connection waiting to authenticate. */
static void
server_auth_callback(int fd, short events, void *data)
{
	struct pending_auth	*pa = data;
	char			 token[65];
	int			 is_tty;
	uint64_t		 now;
	struct timeval		 tv;

	if (events & EV_READ) {
		switch (win32_ipc_read_auth(fd, &pa->auth, token, sizeof token,
		    &is_tty)) {
		case 0:
			server_free_pending_auth(pa, 0);
			server_accept_authenticated(fd, is_tty, token);
			return;
		case -1:
			log_debug("connection %d failed authentication", fd);
			server_free_pending_auth(pa, 1);
			return;
		}
	}

	now = get_timer();
	if (now >= pa->deadline) {
		log_debug("connection %d did not authenticate in time", fd);
		server_free_pending_auth(pa, 1);
		return;
	}
	tv.tv_sec = (pa->deadline - now) / 1000;
	tv.tv_usec = ((pa->deadline - now) % 1000) * 1000;
	event_add(&pa->event, &tv);
}

/* Start waiting for a new connection to authenticate. */
static void
server_add_pending_auth(int fd)
{
	struct pending_auth	*pa;
	struct timeval		 tv = { .tv_sec = PENDING_AUTH_TIMEOUT };

	/* Make room by dropping the oldest if there are too many. */
	if (npending_auths >= MAX_PENDING_AUTHS) {
		pa = TAILQ_FIRST(&pending_auths);
		log_debug("too many pending connections, dropping %d", pa->fd);
		server_free_pending_auth(pa, 1);
	}

	setblocking(fd, 0);
	pa = xcalloc(1, sizeof *pa);
	pa->fd = fd;
	pa->deadline = get_timer() + PENDING_AUTH_TIMEOUT * 1000;
	event_set(&pa->event, fd, EV_READ, server_auth_callback, pa);
	event_add(&pa->event, &tv);
	TAILQ_INSERT_TAIL(&pending_auths, pa, entry);
	npending_auths++;
}

/* A connection has authenticated: it is a new client or a tty channel. */
static void
server_accept_authenticated(int fd, int is_tty, const char *token)
{
	struct client	*c;

	if (server_exit) {
		closesocket((SOCKET)fd);
		return;
	}

	if (is_tty) {
		/*
		 * Find the client already waiting for this channel or keep it
		 * until the client identifies.
		 */
		TAILQ_FOREACH(c, &clients, entry) {
			if (c->tty_token != NULL &&
			    strcmp(c->tty_token, token) == 0) {
				server_client_set_tty_fd(c, fd);
				return;
			}
		}
		server_add_pending_tty(token, fd);
		return;
	}

	if (server_first_deadline != 0) {
		server_first_deadline = 0;
		evtimer_del(&server_ev_first);
	}
	c = server_client_create(fd);
	if (!server_acl_join(c)) {
		c->exit_message = xstrdup("access not allowed");
		c->flags |= CLIENT_EXIT;
	}
}
#endif

struct cmd_find_state	 marked_pane;

static u_int		 message_next;
struct message_list	 message_log;

time_t			 current_time;

static int	server_loop(void);
static void	server_send_exit(void);
static void	server_accept(int, short, void *);
static void	server_signal(int);
static void	server_child_signal(void);
static void	server_child_exited(pid_t, int);
static void	server_child_stopped(pid_t, int);

/* Set marked pane. */
void
server_set_marked(struct session *s, struct winlink *wl, struct window_pane *wp)
{
	cmd_find_clear_state(&marked_pane, 0);
	marked_pane.s = s;
	marked_pane.wl = wl;
	if (wl != NULL)
		marked_pane.w = wl->window;
	marked_pane.wp = wp;
}

/* Clear marked pane. */
void
server_clear_marked(void)
{
	cmd_find_clear_state(&marked_pane, 0);
}

/* Is this the marked pane? */
int
server_is_marked(struct session *s, struct winlink *wl, struct window_pane *wp)
{
	if (s == NULL || wl == NULL || wp == NULL)
		return (0);
	if (marked_pane.s != s || marked_pane.wl != wl)
		return (0);
	if (marked_pane.wp != wp)
		return (0);
	return (server_check_marked());
}

/* Check if the marked pane is still valid. */
int
server_check_marked(void)
{
	return (cmd_find_valid_state(&marked_pane));
}

/* Create server socket. */
int
server_create_socket(uint64_t flags, char **cause)
{
#ifdef _WIN32
	int	 fd;
	char	*error = NULL;

	(void)flags;
	fd = win32_ipc_create_server(socket_path, &error);
	if (fd == -1) {
		if (cause != NULL)
			*cause = error;
		else
			free(error);
		return (-1);
	}
	setblocking(fd, 0);
	return (fd);
#else
	struct sockaddr_un	sa;
	size_t			size;
	mode_t			mask;
	int			fd, saved_errno;

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	size = strlcpy(sa.sun_path, socket_path, sizeof sa.sun_path);
	if (size >= sizeof sa.sun_path) {
		errno = ENAMETOOLONG;
		goto fail;
	}
	unlink(sa.sun_path);

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		goto fail;

	if (flags & CLIENT_DEFAULTSOCKET)
		mask = umask(S_IXUSR|S_IXGRP|S_IRWXO);
	else
		mask = umask(S_IXUSR|S_IRWXG|S_IRWXO);
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) == -1) {
		saved_errno = errno;
		close(fd);
		errno = saved_errno;
		goto fail;
	}
	umask(mask);

	if (listen(fd, 128) == -1) {
		saved_errno = errno;
		close(fd);
		errno = saved_errno;
		goto fail;
	}
	setblocking(fd, 0);

	return (fd);

fail:
	if (cause != NULL) {
		xasprintf(cause, "error creating %s (%s)", socket_path,
		    strerror(errno));
	}
	return (-1);
#endif
}

/* Tidy up every hour. */
static void
server_tidy_event(__unused int fd, __unused short events, __unused void *data)
{
    struct timeval	tv = { .tv_sec = 3600 };
    uint64_t		t = get_timer();

    format_tidy_jobs();

#ifdef HAVE_MALLOC_TRIM
    malloc_trim(0);
#endif

    log_debug("%s: took %llu milliseconds", __func__,
        (unsigned long long)(get_timer() - t));
    evtimer_add(&server_ev_tidy, &tv);
}

/* Fork new server. */
int
server_start(struct tmuxproc *client, uint64_t flags, struct event_base *base,
    int lockfd, char *lockfile)
{
	int		 fd;
#ifndef _WIN32
	sigset_t	 set, oldset;
#endif
	struct client	*c = NULL;
	char		*cause = NULL;
	struct timeval	 tv = { .tv_sec = 3600 };
#ifdef _WIN32
	struct timeval	 first_tv = { .tv_sec = SERVER_FIRST_CLIENT };
	const char	*home;
#endif

#ifndef _WIN32
	sigfillset(&set);
	sigprocmask(SIG_BLOCK, &set, &oldset);
#endif

	if (~flags & CLIENT_NOFORK) {
		if (proc_fork_and_daemon(&fd) != 0) {
#ifndef _WIN32
			sigprocmask(SIG_SETMASK, &oldset, NULL);
#endif
			return (fd);
		}
	}
	proc_clear_signals(client, 0);
	server_client_flags = flags;

	if (event_reinit(base) != 0)
		fatalx("event_reinit failed");
	server_proc = proc_start("server");

	proc_set_signals(server_proc, server_signal);
#ifndef _WIN32
	sigprocmask(SIG_SETMASK, &oldset, NULL);
#endif

	if (log_get_level() > 1)
		tty_create_log();
#ifdef _WIN32
	/*
	 * A server started by a client begins in the client's directory, so
	 * log files end up there as they would on Unix. Move away so that the
	 * directory is not held open (and so cannot be removed) while the
	 * server runs.
	 */
	if (win32_server_autostart && (home = find_home()) != NULL &&
	    chdir(home) != 0)
		log_debug("can't change directory to %s", home);
#endif
	if (pledge("stdio rpath wpath cpath fattr unix getpw recvfd proc exec "
	    "tty ps", NULL) != 0)
		fatal("pledge failed");

	input_key_build();
	utf8_update_width_cache();
	RB_INIT(&windows);
	RB_INIT(&all_window_panes);
	TAILQ_INIT(&clients);
	RB_INIT(&sessions);
	key_bindings_init();
	TAILQ_INIT(&message_log);
	gettimeofday(&start_time, NULL);

#ifdef HAVE_SYSTEMD
	server_fd = systemd_create_socket(flags, &cause);
#else
	server_fd = server_create_socket(flags, &cause);
#endif
	if (server_fd != -1)
		server_update_socket();
	if (~flags & CLIENT_NOFORK)
		c = server_client_create(fd);
#ifdef _WIN32
	/*
	 * Servers started by clients run with -D as well, but they should exit
	 * when empty like a forked server on Unix; only a user's own -D server
	 * stays.
	 */
	else if (!win32_server_autostart)
#else
	else
#endif
		options_set_number(global_options, "exit-empty", 0);

	if (lockfd >= 0) {
		unlink(lockfile);
		free(lockfile);
		close(lockfd);
	}

	if (cause != NULL) {
		if (c != NULL) {
			c->exit_message = cause;
			c->flags |= CLIENT_EXIT;
		} else {
			fprintf(stderr, "%s\n", cause);
			exit(1);
		}
	}

	evtimer_set(&server_ev_tidy, server_tidy_event, NULL);
	evtimer_add(&server_ev_tidy, &tv);

	server_acl_init();

#ifdef _WIN32
	/*
	 * A server started by a client has no client yet; give the client
	 * time to connect before exiting for being empty (and wake up then,
	 * in case it never does).
	 */
	if (win32_server_autostart) {
		server_first_deadline = get_timer() + SERVER_FIRST_CLIENT * 1000;
		evtimer_set(&server_ev_first, server_first_event, NULL);
		evtimer_add(&server_ev_first, &first_tv);
	}
#endif

	server_add_accept(0);
	proc_loop(server_proc, server_loop);

	job_kill_all();
	prompt_save_history();

#ifdef _WIN32
	/* Shut down the discovery pipe and accept thread. */
	win32_ipc_cleanup();
#endif

	exit(0);
}

/* Server loop callback. */
static int
server_loop(void)
{
	struct client	*c;
	u_int		 items;

	current_time = time(NULL);

	do {
		items = cmdq_next(NULL);
		TAILQ_FOREACH(c, &clients, entry) {
			if (c->flags & CLIENT_IDENTIFIED)
				items += cmdq_next(c);
		}
	} while (items != 0);

	server_client_loop();

	if (!options_get_number(global_options, "exit-empty") && !server_exit)
		return (0);

	if (!options_get_number(global_options, "exit-unattached")) {
		if (!RB_EMPTY(&sessions))
			return (0);
	}

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session != NULL)
			return (0);
	}

	/*
	 * No attached clients therefore want to exit - flush any waiting
	 * clients but don't actually exit until they've gone.
	 */
	cmd_wait_for_flush();
	if (!TAILQ_EMPTY(&clients))
		return (0);
#ifdef _WIN32
	/*
	 * A connection still authenticating may be the first client, and a
	 * server started by a client waits a while for it to connect.
	 */
	if (!server_exit && (npending_auths != 0 ||
	    (server_first_deadline != 0 && get_timer() < server_first_deadline)))
		return (0);
#endif

	if (job_still_running())
		return (0);

	log_debug("server_loop: exiting (exit-empty=%lld exit-unatt=%lld "
	    "sessions=%d server_exit=%d)",
	    (long long)options_get_number(global_options, "exit-empty"),
	    (long long)options_get_number(global_options, "exit-unattached"),
	    !RB_EMPTY(&sessions), server_exit);
	return (1);
}

/* Exit the server by killing all clients and windows. */
static void
server_send_exit(void)
{
	struct client	*c, *c1;
	struct session	*s, *s1;

	cmd_wait_for_flush();

	TAILQ_FOREACH_SAFE(c, &clients, entry, c1) {
		if (c->flags & CLIENT_SUSPENDED)
			server_client_lost(c);
		else {
			c->flags |= CLIENT_EXIT;
			c->exit_type = CLIENT_EXIT_SHUTDOWN;
		}
		c->session = NULL;
	}

	RB_FOREACH_SAFE(s, sessions, &sessions, s1)
		session_destroy(s, 1, __func__);
}

/* Update socket execute permissions based on whether sessions are attached. */
void
server_update_socket(void)
{
#ifdef _WIN32
	/* No socket file permissions to manage on Windows. */
#else
	struct session	*s;
	static int	 last = -1;
	int		 n, mode;
	struct stat      sb;

	n = 0;
	RB_FOREACH(s, sessions, &sessions) {
		if (s->attached != 0) {
			n++;
			break;
		}
	}

	if (n != last) {
		last = n;

		if (stat(socket_path, &sb) != 0)
			return;
		mode = sb.st_mode & ACCESSPERMS;
		if (n != 0) {
			if (mode & S_IRUSR)
				mode |= S_IXUSR;
			if (mode & S_IRGRP)
				mode |= S_IXGRP;
			if (mode & S_IROTH)
				mode |= S_IXOTH;
		} else
			mode &= ~(S_IXUSR|S_IXGRP|S_IXOTH);
		chmod(socket_path, mode);
	}
#endif
}

/* Callback for server socket. */
static void
server_accept(int fd, short events, __unused void *data)
{
#ifdef _WIN32
	int	newfd, i;

	server_add_accept(0);
	if (!(events & EV_READ))
		return;

	/*
	 * Accept everything that is waiting (within reason). New connections
	 * must first authenticate with a nonce from the discovery pipe; that
	 * is read asynchronously so a silent peer cannot block the server.
	 */
	for (i = 0; i < 32; i++) {
		newfd = win32_ipc_accept(fd);
		if (newfd == -1) {
			if (errno == EAGAIN)
				return;
			/*
			 * Out of resources or an unexpected error: delete and
			 * don't try again for 1 second.
			 */
			server_add_accept(1);
			return;
		}
		if (server_exit) {
			closesocket((SOCKET)newfd);
			continue;
		}
		server_add_pending_auth(newfd);
	}
#else
	struct sockaddr_storage	 sa;
	socklen_t		 slen = sizeof sa;
	int			 newfd;
	struct client		*c;

	server_add_accept(0);
	if (!(events & EV_READ))
		return;

	newfd = accept(fd, (struct sockaddr *) &sa, &slen);
	if (newfd == -1) {
		if (errno == EAGAIN || errno == EINTR || errno == ECONNABORTED)
			return;
		if (errno == ENFILE || errno == EMFILE) {
			/* Delete and don't try again for 1 second. */
			server_add_accept(1);
			return;
		}
		fatal("accept failed");
	}
	if (server_exit) {
		close(newfd);
		return;
	}
	c = server_client_create(newfd);
	if (!server_acl_join(c)) {
		c->exit_message = xstrdup("access not allowed");
		c->flags |= CLIENT_EXIT;
	}
#endif
}

/*
 * Add accept event. If timeout is nonzero, add as a timeout instead of a read
 * event - used to backoff when running out of file descriptors.
 */
void
server_add_accept(int timeout)
{
	struct timeval tv = { timeout, 0 };

	if (server_fd == -1)
		return;

	if (event_initialized(&server_ev_accept))
		event_del(&server_ev_accept);

	if (timeout == 0) {
		event_set(&server_ev_accept, server_fd, EV_READ, server_accept,
		    NULL);
		event_add(&server_ev_accept, NULL);
	} else {
		event_set(&server_ev_accept, server_fd, EV_TIMEOUT,
		    server_accept, NULL);
		event_add(&server_ev_accept, &tv);
	}
}

/* Signal handler. */
static void
server_signal(int sig)
{
#ifndef _WIN32
	int	fd;
#endif

	log_debug("%s: %s", __func__, strsignal(sig));
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		server_exit = 1;
		server_send_exit();
		break;
	case SIGCHLD:
		server_child_signal();
		break;
#ifndef _WIN32
	case SIGUSR1:
		event_del(&server_ev_accept);
		fd = server_create_socket(server_client_flags, NULL);
		if (fd != -1) {
			close(server_fd);
			server_fd = fd;
			server_update_socket();
		}
		server_add_accept(0);
		break;
	case SIGUSR2:
		proc_toggle_log(server_proc);
		break;
#endif
	case SIGWINCH:
		/* On Windows, SIGWINCH from resize polling. */
		break;
	}
}

/* Handle SIGCHLD. */
static void
server_child_signal(void)
{
	int	 status;
	pid_t	 pid;

	for (;;) {
		switch (pid = waitpid(WAIT_ANY, &status, WNOHANG|WUNTRACED)) {
		case -1:
			if (errno == ECHILD)
				return;
#ifndef _WIN32
			fatal("waitpid failed");
#else
			return;
#endif
		case 0:
			return;
		}
		if (WIFSTOPPED(status))
			server_child_stopped(pid, status);
		else if (WIFEXITED(status) || WIFSIGNALED(status))
			server_child_exited(pid, status);
	}
}

/* Handle exited children. */
static void
server_child_exited(pid_t pid, int status)
{
	struct window		*w, *w1;
	struct window_pane	*wp;

	RB_FOREACH_SAFE(w, windows, &windows, w1) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->pid == pid) {
				wp->status = status;
				wp->flags |= PANE_STATUSREADY;

				log_debug("%%%u exited", wp->id);
				wp->flags |= PANE_EXITED;

				window_pane_wait_finish(wp);
				spawn_editor_finish(wp);

				if (window_pane_destroy_ready(wp))
					server_destroy_pane(wp, 1);
				break;
			}
		}
	}
	job_check_died(pid, status);
}

/* Handle stopped children. */
static void
server_child_stopped(pid_t pid, int status)
{
	struct window		*w;
	struct window_pane	*wp;

	if (WSTOPSIG(status) == SIGTTIN || WSTOPSIG(status) == SIGTTOU)
		return;

	RB_FOREACH(w, windows, &windows) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->pid == pid) {
				if (killpg(pid, SIGCONT) != 0)
					kill(pid, SIGCONT);
			}
		}
	}
	job_check_died(pid, status);
}

/* Add to message log. */
void
server_add_message(const char *fmt, ...)
{
	struct message_entry	*msg, *msg1;
	char			*s;
	va_list			 ap;
	u_int			 limit;

	va_start(ap, fmt);
	xvasprintf(&s, fmt, ap);
	va_end(ap);

	log_debug("message: %s", s);

	msg = xcalloc(1, sizeof *msg);
	gettimeofday(&msg->msg_time, NULL);
	msg->msg_num = message_next++;
	msg->msg = s;
	TAILQ_INSERT_TAIL(&message_log, msg, entry);

	limit = options_get_number(global_options, "message-limit");
	TAILQ_FOREACH_SAFE(msg, &message_log, entry, msg1) {
		if (msg->msg_num + limit >= message_next)
			break;
		free(msg->msg);
		TAILQ_REMOVE(&message_log, msg, entry);
		free(msg);
	}
}
