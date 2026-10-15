/* $OpenBSD$ */

/*
 * Copyright (c) 2009 Nicholas Marriott <nicholas.marriott@gmail.com>
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

#include <ctype.h>
#ifndef _WIN32
#include <libgen.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

static void	 name_time_callback(evutil_socket_t, short, void *);
static int	 name_time_expired(struct window *, struct timeval *);

static char	*format_window_name(struct window *);

#ifdef _WIN32
/*
 * A Windows pane gives no sign when its foreground process changes without
 * output: MSYS2 bash forks and execs a command well after echoing it, so the
 * check made for the echo may still see bash. While any window has
 * automatic-rename on, check all of them again every this many seconds. One
 * timer for every window means one look at the process table per interval,
 * and leaves each window's own timer free for the usual rate limit.
 */
#define NAME_RECHECK_INTERVAL 1

static struct event	name_recheck_event;

static void
name_recheck_callback(__unused evutil_socket_t fd, __unused short events,
    __unused void *arg)
{
	struct window	*w;

	/* The event loop will call check_window_name for us on the way out. */
	RB_FOREACH(w, windows, &windows) {
		if (w->active != NULL &&
		    options_get_number(w->options, "automatic-rename"))
			w->active->flags |= PANE_CHANGED;
	}
}

static void
name_recheck_queue(void)
{
	struct timeval	tv = { NAME_RECHECK_INTERVAL, 0 };

	if (!event_initialized(&name_recheck_event))
		evtimer_set(&name_recheck_event, name_recheck_callback, NULL);
	if (!evtimer_pending(&name_recheck_event, NULL))
		evtimer_add(&name_recheck_event, &tv);
}
#endif

static void
name_time_callback(__unused evutil_socket_t fd, __unused short events,
    void *arg)
{
	struct window	*w = arg;

	/* The event loop will call check_window_name for us on the way out. */
	log_debug("@%u name timer expired", w->id);
}

static int
name_time_expired(struct window *w, struct timeval *tv)
{
	struct timeval	offset;

	timersub(tv, &w->name_time, &offset);
	if (offset.tv_sec != 0 || offset.tv_usec > NAME_INTERVAL)
		return (0);
	return (NAME_INTERVAL - offset.tv_usec);
}

void
check_window_name(struct window *w)
{
	struct timeval	 tv, next;
	char		*name;
	int		 left;

	if (w->active == NULL)
		return;

	if (!options_get_number(w->options, "automatic-rename"))
		return;
#ifdef _WIN32
	name_recheck_queue();
#endif

	if (~w->active->flags & PANE_CHANGED) {
		log_debug("@%u active pane not changed", w->id);
		return;
	}
	log_debug("@%u active pane changed", w->id);

	gettimeofday(&tv, NULL);
	left = name_time_expired(w, &tv);
	if (left != 0) {
		if (!event_initialized(&w->name_event))
			evtimer_set(&w->name_event, name_time_callback, w);
		if (!evtimer_pending(&w->name_event, NULL)) {
			log_debug("@%u name timer queued (%d left)", w->id,
			    left);
			timerclear(&next);
			next.tv_usec = left;
			event_add(&w->name_event, &next);
		} else {
			log_debug("@%u name timer already queued (%d left)",
			    w->id, left);
		}
		return;
	}
	memcpy(&w->name_time, &tv, sizeof w->name_time);
	if (event_initialized(&w->name_event))
		evtimer_del(&w->name_event);

	w->active->flags &= ~PANE_CHANGED;

	name = format_window_name(w);
	if (strcmp(name, w->name) != 0) {
		log_debug("@%u new name %s (was %s)", w->id, name, w->name);
		window_set_name(w, name, WINDOW_NAME_FORBID_EXT);
		server_redraw_window_borders(w);
		server_status_window(w);
	} else
		log_debug("@%u name not changed (still %s)", w->id, w->name);

	free(name);
}

char *
default_window_name(struct window *w)
{
	char	*cmd, *s;

	if (w->active == NULL)
		return (xstrdup(""));
	cmd = cmd_stringify_argv(w->active->argc, w->active->argv);
	if (cmd != NULL && *cmd != '\0')
		s = parse_window_name(cmd);
	else
		s = parse_window_name(w->active->shell);
	free(cmd);
	return (s);
}

static char *
format_window_name(struct window *w)
{
	struct format_tree	*ft;
	const char		*fmt;
	char			*name;

	ft = format_create(NULL, NULL, FORMAT_WINDOW|w->id, 0);
	format_defaults_window(ft, w);
	format_defaults_pane(ft, w->active);

	fmt = options_get_string(w->options, "automatic-rename-format");
	name = format_expand(ft, fmt);

	format_free(ft);
	return (name);
}

#ifdef _WIN32
/* Does the first len bytes of name end with an executable extension? */
static int
parse_window_name_win32_ext(const char *name, size_t len)
{
	static const char	*exts[] = { ".exe", ".com", ".bat", ".cmd" };
	u_int			 i;

	if (len < 4)
		return (0);
	for (i = 0; i < nitems(exts); i++) {
		if (strncasecmp(name + len - 4, exts[i], 4) == 0)
			return (1);
	}
	return (0);
}

/*
 * Remove the quotes args_escape() puts around an argument containing spaces
 * or quotes, undoing its backslash escapes. Anything after the closing quote
 * (the remaining arguments) is dropped.
 */
static char *
parse_window_name_win32_unquote(char *name)
{
	char	 quote = *name, *in, *out;

	if (quote != '"' && quote != '\'')
		return (name);
	name++;
	for (in = out = name; *in != '\0' && *in != quote; in++) {
		if (in[0] == '\\' && in[1] != '\0' &&
		    strchr(quote == '"' ? "\\\"$`" : "\\", in[1]) != NULL)
			in++;
		*out++ = *in;
	}
	*out = '\0';
	return (name);
}

/*
 * Find the program in a Windows command line or path. The program may be
 * quoted ("C:\Program Files\Git\bin\bash.exe" -l) or, as with a shell path
 * or a single argument, an unquoted path containing spaces.
 */
static char *
parse_window_name_win32(char *name)
{
	char	*ptr;

	name = parse_window_name_win32_unquote(name);

	if (strncmp(name, "exec ", (sizeof "exec ") - 1) == 0)
		name = name + (sizeof "exec ") - 1;
	while (*name == ' ' || *name == '-')
		name++;

	if (*name == '"') {
		name++;
		name[strcspn(name, "\"")] = '\0';
		return (name);
	}

	/*
	 * In an unquoted absolute path, the program ends at the first space
	 * after an executable extension, or is the whole string if it ends
	 * with one.
	 */
	if ((isalpha((u_char)name[0]) && name[1] == ':') ||
	    strncmp(name, "\\\\", 2) == 0) {
		for (ptr = strchr(name, ' '); ptr != NULL;
		    ptr = strchr(ptr + 1, ' ')) {
			if (parse_window_name_win32_ext(name, ptr - name)) {
				*ptr = '\0';
				return (name);
			}
		}
		if (parse_window_name_win32_ext(name, strlen(name)))
			return (name);
	}
	if ((ptr = strchr(name, ' ')) != NULL)
		*ptr = '\0';
	return (name);
}
#endif

char *
parse_window_name(const char *in)
{
	char	*copy, *name, *ptr;

	name = copy = xstrdup(in);
#ifdef _WIN32
	name = parse_window_name_win32(name);
#else
	if (*name == '"')
		name++;
	name[strcspn(name, "\"")] = '\0';

	if (strncmp(name, "exec ", (sizeof "exec ") - 1) == 0)
		name = name + (sizeof "exec ") - 1;

	while (*name == ' ' || *name == '-')
		name++;
	if ((ptr = strchr(name, ' ')) != NULL)
		*ptr = '\0';
#endif

	if (*name != '\0') {
		ptr = name + strlen(name) - 1;
		while (ptr > name &&
		    !isalnum((u_char)*ptr) &&
		    !ispunct((u_char)*ptr))
			*ptr-- = '\0';
	}

#ifdef _WIN32
	if (*name == '/' || strchr(name, '\\') != NULL ||
	    (isalpha((u_char)name[0]) && name[1] == ':')) {
		char *slash = strrchr(name, '/');
		char *bslash = strrchr(name, '\\');
		if (bslash != NULL && (slash == NULL || bslash > slash))
			slash = bslash;
		if (slash == NULL && name[1] == ':')
			slash = name + 1;
		if (slash != NULL && slash[1] != '\0')
			name = slash + 1;
	}
	if (strlen(name) > 4 && strcasecmp(name + strlen(name) - 4, ".exe") == 0)
		name[strlen(name) - 4] = '\0';
#else
	if (*name == '/')
		name = basename(name);
#endif
	name = clean_name(name, WINDOW_NAME_FORBID);
	free(copy);
	if (name == NULL)
		return (xstrdup(""));
	return (name);
}
