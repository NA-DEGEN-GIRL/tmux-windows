#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *tparm(const char *, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t,
    intptr_t, intptr_t, intptr_t, intptr_t);
char *tiparm_s(int, int, const char *, ...);

void
win32_signal_notify(int signo)
{
	(void)signo;
}

static int
check(const char *name, const char *actual, const char *expected)
{
	if (actual != NULL && strcmp(actual, expected) == 0)
		return (0);
	fprintf(stderr, "%s: expected %s, got %s\n", name, expected,
	    actual == NULL ? "(null)" : actual);
	return (1);
}

#define T0(s) tparm(s, 0, 0, 0, 0, 0, 0, 0, 0, 0)
#define T1(s, a) tparm(s, a, 0, 0, 0, 0, 0, 0, 0, 0)
#define T2(s, a, b) tparm(s, a, b, 0, 0, 0, 0, 0, 0, 0)

/* Capabilities from the tmux terminal features and xterm-256color. */
#define SETAF "\033[%?%p1%{8}%<%t3%p1%d%e%p1%{16}%<%t9%p1%{8}%-%d%e38;5;%p1%d%;m"
#define SETULC "\033[58::2::%p1%{65536}%/%d::%p1%{256}%/%{255}%&%d::%p1%{255}%&%d%;m"
#define HLS "\033]8;%?%p1%l%tid=%p1%s%;;%p2%s\033\\"
#define MS "\033]52;%p1%s;%p2%s\a"
#define SYNC "\033[?2026%?%p1%{1}%-%tl%eh%;"

int
main(void)
{
	static const char link[] = "https://example.test/status";
	static const char id[] = "id=codex-status";
	int		 errors = 0;
	size_t		 big_len = 300000, i;
	char		*big, *expected;

	if ((uintptr_t)link <= UINT32_MAX || (uintptr_t)id <= UINT32_MAX) {
		fprintf(stderr, "test strings are not above the 32-bit address range\n");
		return (1);
	}

	errors += check("string parameters",
	    tparm("\033]8;%p1%s;%p2%s\033\\", (intptr_t)id, (intptr_t)link,
	    0, 0, 0, 0, 0, 0, 0),
	    "\033]8;id=codex-status;https://example.test/status\033\\");
	errors += check("integer parameters",
	    tparm("\033[%i%p1%d;%p2%dH", 0, 0, 0, 0, 0, 0, 0, 0, 0),
	    "\033[1;1H");

	/* Conditionals, including else-if chains. */
	errors += check("setaf 1", T1(SETAF, 1), "\033[31m");
	errors += check("setaf 9", T1(SETAF, 9), "\033[91m");
	errors += check("setaf 200", T1(SETAF, 200), "\033[38;5;200m");
	errors += check("sync begin", T1(SYNC, 1), "\033[?2026h");
	errors += check("sync end", T1(SYNC, 2), "\033[?2026l");
	errors += check("nested", T2("%?%p1%t[%?%p2%tA%eB%;]%e-%;", 1, 0),
	    "[B]");
	errors += check("nested false", T2("%?%p1%t[%?%p2%tA%eB%;]%e-%;", 0, 1),
	    "-");
	errors += check("percent in skip", T1("%?%p1%t%%;x%;y", 0), "y");

	/* Arithmetic and bit operations. */
	errors += check("setulc", T1(SETULC, 0x123456),
	    "\033[58::2::18::52::86m");
	errors += check("ops", T2("%p1%p2%+%d,%p1%p2%-%d,%p1%p2%*%d,"
	    "%p1%p2%/%d,%p1%p2%m%d", 17, 5), "22,12,85,3,2");
	errors += check("bits", T2("%p1%p2%&%d,%p1%p2%|%d,%p1%p2%^%d,%p1%~%d",
	    12, 10), "8,14,6,-13");
	errors += check("logic", T2("%p1%p2%A%d%p1%p2%O%d%p1%!%d%p1%p2%>%d"
	    "%p1%p2%<%d%p1%p2%=%d", 3, 0), "010100");
	errors += check("divide by zero", T2("%p1%p2%/%d%p1%p2%m%d", 7, 0),
	    "00");
	errors += check("constants", T0("%{12}%{-3}%+%d %'A'%c %'z'%d"),
	    "9 A 122");
	errors += check("variables", T1("%p1%Pa%ga%ga%*%d", 7), "49");
	errors += check("static variables", T1("%p1%PZ%gZ%d", 5), "5");

	/* printf-style output. */
	errors += check("printf", T1("[%p1%4d][%p1%04d][%p1%x][%p1%X][%p1%o]"
	    "[%p1%:-5d][%p1%:+d]", 255), "[ 255][0255][ff][FF][377][255  ][+255]");
	errors += check("printf string",
	    tparm("[%p1%5s][%p1%:-6s][%p1%.1s]", (intptr_t)"ab", 0, 0, 0, 0, 0,
	    0, 0, 0), "[   ab][ab    ][a]");
	errors += check("char", T1("%p1%c", 'Q'), "Q");

	/* String parameters and %l. */
	errors += check("hyperlink with id",
	    tparm(HLS, (intptr_t)"x", (intptr_t)link, 0, 0, 0, 0, 0, 0, 0),
	    "\033]8;id=x;https://example.test/status\033\\");
	errors += check("hyperlink without id",
	    tparm(HLS, (intptr_t)"", (intptr_t)link, 0, 0, 0, 0, 0, 0, 0),
	    "\033]8;;https://example.test/status\033\\");
	errors += check("tiparm_s hyperlink",
	    tiparm_s(2, 3, HLS, "y", "z"), "\033]8;id=y;z\033\\");
	errors += check("tiparm_s cursor style", tiparm_s(1, 0, "\033[%p1%d q",
	    5), "\033[5 q");
	errors += check("tiparm_s cursor colour", tiparm_s(1, 1,
	    "\033]12;%p1%s\a", "#ff0000"), "\033]12;#ff0000\a");
	if (tiparm_s(1, 0, "\033[%p1%s q", 5) != NULL) {
		fprintf(stderr, "tiparm_s accepted a number used as a string\n");
		errors++;
	}

	/* A long payload (a large OSC 52 copy) must not be truncated. */
	big = malloc(big_len + 1);
	expected = malloc(big_len + 32);
	if (big == NULL || expected == NULL)
		return (1);
	for (i = 0; i < big_len; i++)
		big[i] = 'A' + (i % 26);
	big[big_len] = '\0';
	snprintf(expected, big_len + 32, "\033]52;c;%s\a", big);
	errors += check("long payload", tiparm_s(2, 3, MS, "c", big), expected);
	free(big);
	free(expected);
	errors += check("short after long", T1("\033[%p1%dX", 3), "\033[3X");

	/* Malformed strings must not read past the end. */
	errors += check("trailing percent", T0("abc%"), "abc");
	errors += check("trailing p", T0("abc%p"), "abc");
	errors += check("bad p", T0("a%p0b"), "a0b");
	errors += check("trailing brace", T0("x%{12"), "x");
	errors += check("trailing quote", T0("x%'"), "x");
	errors += check("trailing t", T0("x%?%t"), "x");
	errors += check("trailing e", T1("x%?%p1%tA%e", 1), "xA");
	errors += check("trailing printf", T0("x%:-"), "x");
	errors += check("unknown", T0("a%zb"), "ab");
	errors += check("underflow", T0("%d%s|"), "0|");
	errors += check("underflow ops", T0("%+%-%*%/%l%d"), "0");

	if (errors != 0)
		return (1);
	puts("Windows tparm tests passed");
	return (0);
}
