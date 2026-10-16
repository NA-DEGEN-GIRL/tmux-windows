/*
 * Unit tests for the Windows text support: strftime and the time
 * functions, fnmatch, the regex engine, UTF-8 decoding and character
 * widths. Non-ASCII text is written as UTF-8 escapes so the file compiles
 * the same in any code page.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "win32-platform.h"

static int	failures;

void
win32_signal_notify(int signo)
{
	(void)signo;
}

static void
check_str(const char *name, const char *actual, const char *expected)
{
	if (actual != NULL && strcmp(actual, expected) == 0)
		return;
	fprintf(stderr, "FAIL %s: expected '%s', got '%s'\n", name, expected,
	    actual == NULL ? "(null)" : actual);
	failures++;
}

static void
check_int(const char *name, long long actual, long long expected)
{
	if (actual == expected)
		return;
	fprintf(stderr, "FAIL %s: expected %lld, got %lld\n", name, expected,
	    actual);
	failures++;
}

/* A fixed time: Tuesday 2024-01-02 03:04:05, day 1 of the year. */
static struct tm
fixed_tm(void)
{
	struct tm	tm;

	memset(&tm, 0, sizeof tm);
	tm.tm_year = 124;
	tm.tm_mon = 0;
	tm.tm_mday = 2;
	tm.tm_hour = 3;
	tm.tm_min = 4;
	tm.tm_sec = 5;
	tm.tm_wday = 2;
	tm.tm_yday = 1;
	return (tm);
}

static void
check_strftime(const char *fmt, const struct tm *tm, const char *expected)
{
	char	buf[256], name[300];
	size_t	n;

	n = strftime(buf, sizeof buf, fmt, tm);
	if (n == 0)
		buf[0] = '\0';
	snprintf(name, sizeof name, "strftime '%s'", fmt);
	check_str(name, buf, expected);
}

static void
test_time(void)
{
	struct tm	tm = fixed_tm(), pm, bad;
	time_t		t;
	char		buf[512];

	check_strftime("%H:%M:%S", &tm, "03:04:05");
	check_strftime("%k|%l|%e", &tm, " 3| 3| 2");
	check_strftime("%-d|%-H|%-I|%_m|%05Y", &tm, "2|3|3| 1|02024");
	check_strftime("%C|%y|%G|%g|%V|%U|%W|%j|%u|%w", &tm,
	    "20|24|2024|24|01|00|01|002|2|2");
	check_strftime("%D|%F|%R|%T", &tm, "01/02/24|2024-01-02|03:04|03:04:05");
	check_strftime("a%nb%tc%%", &tm, "a\nb\tc%");
	check_strftime("50%", &tm, "50%");
	check_strftime("%", &tm, "%");
	check_strftime("% x%q%-q%5", &tm, "% x%q%-q%5");
	check_strftime("%Ey|%OH", &tm, "24|03");
	check_strftime("%10H|%-10H|%_3d", &tm, "0000000003|3|  2");

	pm = tm;
	pm.tm_hour = 15;
	check_strftime("%I|%l|%k", &pm, "03| 3|15");

	/* ISO week at year boundaries: 2021-01-01 is in 2020-W53. */
	memset(&tm, 0, sizeof tm);
	tm.tm_year = 121;
	tm.tm_mday = 1;
	tm.tm_wday = 5;
	tm.tm_yday = 0;
	check_strftime("%G-W%V-%u", &tm, "2020-W53-5");

	/* Fields out of range must not abort. */
	memset(&bad, 0xff, sizeof bad);
	if (strftime(buf, sizeof buf, "%a %b %c %x %X %r %p %Z %z %H %j",
	    &bad) == 0)
		buf[0] = '\0';
	check_str("strftime bad tm names", strncmp(buf, "? ? ? ? -1 ", 11) == 0 ?
	    "ok" : buf, "ok");
	if (strftime(buf, sizeof buf, "%P%s%G%V%U%W%C", &bad) == 0)
		buf[0] = '\0';

	/* Output that does not fit returns 0. */
	check_int("strftime too long", strftime(buf, 4, "%Y-%m", &pm), 0);

	/* localtime_r and gmtime_r for times the UCRT rejects. */
	t = 0;
	check_str("gmtime_r epoch", gmtime_r(&t, &tm) != NULL ? "ok" : "null",
	    "ok");
	check_int("gmtime_r epoch wday", tm.tm_wday, 4);
	t = -86400;
	gmtime_r(&t, &tm);
	check_int("gmtime_r 1969 year", tm.tm_year, 69);
	check_int("gmtime_r 1969 mday", tm.tm_mday, 31);
	check_int("gmtime_r 1969 wday", tm.tm_wday, 3);
	check_int("gmtime_r 1969 yday", tm.tm_yday, 364);
	t = 32503680000LL;	/* 3000-01-01 */
	gmtime_r(&t, &tm);
	check_int("gmtime_r 3000 year", tm.tm_year, 1100);
	check_int("gmtime_r 3000 wday", tm.tm_wday, 3);
	t = 253402300800LL;	/* 10000-01-01 */
	localtime_r(&t, &tm);
	check_str("localtime_r huge in range",
	    (tm.tm_mon >= 0 && tm.tm_mon <= 11 && tm.tm_wday >= 0 &&
	    tm.tm_wday <= 6) ? "ok" : "bad", "ok");
	t = INT64_MAX;
	check_str("localtime_r overflow", localtime_r(&t, &tm) == NULL ?
	    "null" : "ok", "null");
	check_int("localtime_r overflow tm", tm.tm_year, 70);
	strftime(buf, sizeof buf, "%H:%M %a %b", &tm);
	t = 0;
	ctime_r(&t, buf);
	check_int("ctime_r length", (long long)strlen(buf), 25);
}

static void
check_fnmatch(const char *pattern, const char *string, int flags,
    int expected)
{
	char	name[300];

	snprintf(name, sizeof name, "fnmatch '%s' '%s' %d", pattern, string,
	    flags);
	check_int(name, fnmatch(pattern, string, flags) == 0, expected);
}

static void
test_fnmatch(void)
{
	char	big[4096];

	check_fnmatch("xterm*", "xterm-256color", 0, 1);
	check_fnmatch("*:RGB", "*:RGB", 0, 1);
	check_fnmatch("*256*", "xterm-256color", 0, 1);
	check_fnmatch("xterm", "xterm-256color", 0, 0);
	check_fnmatch("a\\*b", "a*b", 0, 1);
	check_fnmatch("a\\*b", "axb", 0, 0);
	check_fnmatch("a\\*b", "a\\xb", FNM_NOESCAPE, 1);
	check_fnmatch("[]a]", "]", 0, 1);
	check_fnmatch("[]a]", "a", 0, 1);
	check_fnmatch("[]a]", "b", 0, 0);
	check_fnmatch("[!]a]", "b", 0, 1);
	check_fnmatch("[a-c]x", "bx", 0, 1);
	check_fnmatch("[a-c]x", "dx", 0, 0);
	check_fnmatch("a[", "a[", 0, 1);
	check_fnmatch("a[b", "a[b", 0, 1);
	check_fnmatch("a[b", "ab", 0, 0);
	check_fnmatch("[[:alpha:]][[:digit:]]", "x5", 0, 1);
	check_fnmatch("[[:alpha:]][[:digit:]]", "55", 0, 0);
	check_fnmatch("[[:upper:]]*", "Hello", 0, 1);
	check_fnmatch("[[:space:]]", " ", 0, 1);
	check_fnmatch("?", "\xed\x95\x9c", 0, 1);
	check_fnmatch("??", "\xed\x95\x9c", 0, 0);
	check_fnmatch("\xed\x95\x9c?", "\xed\x95\x9c\xea\xb8\x80", 0, 1);
	check_fnmatch("[\xea\xb0\x80-\xed\x9e\xa3]", "\xea\xb8\x80", 0, 1);
	check_fnmatch("[\xea\xb0\x80-\xed\x9e\xa3]", "a", 0, 0);
	check_fnmatch("*\xf0\x9f\x98\x80", "smile \xf0\x9f\x98\x80", 0, 1);
	check_fnmatch("[[:alpha:]]", "\xed\x95\x9c", 0, 1);
	check_fnmatch("HELLO", "hello", FNM_CASEFOLD, 1);
	check_fnmatch("[A-Z]ello", "hello", FNM_CASEFOLD, 1);
	check_fnmatch("hello", "HELLO", 0, 0);
	check_fnmatch("*.c", "dir/file.c", FNM_PATHNAME, 0);
	check_fnmatch("*/*.c", "dir/file.c", FNM_PATHNAME, 1);
	check_fnmatch("*", ".hidden", FNM_PERIOD, 0);
	check_fnmatch("?", "\xff", 0, 1);
	check_fnmatch("a*b*c", "aXXbYYc", 0, 1);
	check_fnmatch("a*b*c", "aXXbYY", 0, 0);
	check_fnmatch("", "", 0, 1);
	check_fnmatch("*", "", 0, 1);

	/* Many stars against a long string must not take forever. */
	memset(big, 'a', sizeof big - 1);
	big[sizeof big - 1] = '\0';
	check_fnmatch("*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*b", big, 0, 0);
}

static const char *
regex_sub(const char *pattern, const char *text, int cflags, int eflags,
    int group)
{
	static char	out[256];
	regex_t		r;
	regmatch_t	m[10];
	int		error;

	error = regcomp(&r, pattern, cflags);
	if (error != 0) {
		snprintf(out, sizeof out, "error %d", error);
		return (out);
	}
	if (regexec(&r, text, 10, m, eflags) != 0)
		snprintf(out, sizeof out, "nomatch");
	else if (m[group].rm_so == -1)
		snprintf(out, sizeof out, "unset");
	else {
		snprintf(out, sizeof out, "%lld-%lld:%.*s",
		    (long long)m[group].rm_so, (long long)m[group].rm_eo,
		    (int)(m[group].rm_eo - m[group].rm_so),
		    text + m[group].rm_so);
	}
	regfree(&r);
	return (out);
}

static void
check_regex(const char *pattern, const char *text, int cflags, int eflags,
    int group, const char *expected)
{
	char	name[300];

	snprintf(name, sizeof name, "regex '%s' on '%s' group %d", pattern,
	    text, group);
	check_str(name, regex_sub(pattern, text, cflags|REG_EXTENDED, eflags,
	    group), expected);
}

static void
test_regex(void)
{
	char		big[20001];
	regex_t		r;
	regmatch_t	m[1];
	DWORD		start;
	char		err[64];

	check_regex("a|y", "xaby", 0, 0, 0, "1-2:a");
	check_regex("b|y", "xaby", 0, 0, 0, "2-3:b");
	check_regex("(ab)+", "xababc", 0, 0, 0, "1-5:abab");
	check_regex("(ab)+", "xababc", 0, 0, 1, "3-5:ab");
	check_regex("(foo)?bar", "bar", 0, 0, 0, "0-3:bar");
	check_regex("(foo)?bar", "foobar", 0, 0, 1, "0-3:foo");
	check_regex("(foo)?bar", "bar", 0, 0, 1, "unset");
	check_regex("a{2,3}", "caaaab", 0, 0, 0, "1-4:aaa");
	check_regex("a{2}", "ab", 0, 0, 0, "nomatch");
	check_regex("[[:digit:]]+", "ab123c", 0, 0, 0, "2-5:123");
	check_regex("[^[:alpha:]]", "ab1", 0, 0, 0, "2-3:1");
	check_regex("((a)b)", "xab", 0, 0, 0, "1-3:ab");
	check_regex("((a)b)", "xab", 0, 0, 1, "1-3:ab");
	check_regex("((a)b)", "xab", 0, 0, 2, "1-2:a");
	check_regex("(a(b(c)))", "abc", 0, 0, 3, "2-3:c");
	check_regex("^", "baaac", 0, 0, 0, "0-0:");
	check_regex("^b", "abab", 0, 0, 0, "nomatch");
	check_regex("b$", "abab", 0, 0, 0, "3-4:b");
	check_regex("^a", "abc", 0, REG_NOTBOL, 0, "nomatch");
	check_regex("c$", "abc", 0, REG_NOTEOL, 0, "nomatch");
	check_regex("a*", "baaac", 0, 0, 0, "0-0:");
	check_regex("a.c", "a\nc", 0, 0, 0, "0-3:a\nc");
	check_regex("a.c", "a\nc", REG_NEWLINE, 0, 0, "nomatch");
	check_regex("^c", "ab\nc", REG_NEWLINE, 0, 0, "3-4:c");
	check_regex("(a|ab)(c|bcd)", "abcd", 0, 0, 0, "0-4:abcd");
	check_regex("HeLLo", "say hello", REG_ICASE, 0, 0, "4-9:hello");
	check_regex("[a-z]+", "ABC", REG_ICASE, 0, 0, "0-3:ABC");
	check_regex("\xc3\xa9", "\xc3\x89", REG_ICASE, 0, 0, "0-2:\xc3\x89");

	/* Hangul: '.' is one character, ranges are code points. */
	check_regex("^\xed\x95\x9c.\xed\x85\x8c", "\xed\x95\x9c\xea\xb8\x80"
	    "\xed\x85\x8c", 0, 0, 0, "0-9:\xed\x95\x9c\xea\xb8\x80\xed\x85\x8c");
	check_regex("^\xed\x95\x9c...$", "\xed\x95\x9c\xea\xb8\x80", 0, 0, 0,
	    "nomatch");
	check_regex("[\xea\xb0\x80-\xed\x9e\xa3]+", "abc\xed\x95\x9c\xea\xb8\x80"
	    "def", 0, 0, 0, "3-9:\xed\x95\x9c\xea\xb8\x80");
	check_regex("[\xea\xb0\x80-\xed\x9e\xa3]", "abc", 0, 0, 0, "nomatch");
	check_regex("[[:alpha:]]+", "1\xed\x95\x9c\xea\xb8\x80!", 0, 0, 0,
	    "1-7:\xed\x95\x9c\xea\xb8\x80");
	check_regex(".", "\xf0\x9f\x98\x80", 0, 0, 0, "0-4:\xf0\x9f\x98\x80");
	check_regex("x.y", "x\xffy", 0, 0, 0, "0-3:x\xffy");

	/* REG_NOSUB with no match array, as format.c uses it. */
	check_int("nosub compile", regcomp(&r, "^x.b", REG_EXTENDED|REG_NOSUB),
	    0);
	check_int("nosub match", regexec(&r, "xaby", 0, NULL, 0), 0);
	check_int("nosub no match", regexec(&r, "yaby", 0, NULL, 0),
	    REG_NOMATCH);
	regfree(&r);
	check_int("nosub hangul compile", regcomp(&r,
	    "^\xed\x95\x9c.\xed\x85\x8c", REG_EXTENDED|REG_NOSUB), 0);
	check_int("nosub hangul match", regexec(&r, "\xed\x95\x9c\xea\xb8\x80"
	    "\xed\x85\x8c\xec\x8a\xa4\xed\x8a\xb8", 0, NULL, 0), 0);
	regfree(&r);

	check_int("regcomp bad", regcomp(&r, "(ab", REG_EXTENDED) != 0, 1);
	check_int("regcomp bad bracket", regcomp(&r, "[ab", REG_EXTENDED) != 0,
	    1);
	check_int("regerror", regerror(REG_NOMATCH, NULL, err, sizeof err) > 1,
	    1);
	check_str("regerror text", err, "No match");

	/* A catastrophic pattern for a backtracking matcher. */
	memset(big, 'a', sizeof big - 1);
	big[sizeof big - 1] = '\0';
	start = GetTickCount();
	if (regcomp(&r, "(a*)*b", REG_EXTENDED) == 0) {
		check_int("catastrophic regex", regexec(&r, big, 1, m, 0),
		    REG_NOMATCH);
		regfree(&r);
	}
	check_int("catastrophic regex time ok", GetTickCount() - start < 5000,
	    1);

	/*
	 * Nested bounded repetitions are expanded into copies, so they must
	 * be refused once they get too big rather than taking minutes and
	 * gigabytes. Patterns of a sensible size still work.
	 */
	start = GetTickCount();
	check_int("nested repetition 2 levels",
	    regcomp(&r, "(a{0,255}){0,255}b", REG_EXTENDED), REG_ESPACE);
	check_int("nested repetition 3 levels",
	    regcomp(&r, "((a{0,255}){0,255}){0,255}b", REG_EXTENDED),
	    REG_ESPACE);
	check_int("nested empty repetition",
	    regcomp(&r, "(((){0,255}){0,255}){0,255}", REG_EXTENDED),
	    REG_ESPACE);
	check_int("nested repetition time ok", GetTickCount() - start < 2000,
	    1);
	check_int("large repetition compile",
	    regcomp(&r, "^[a-z]{1,255}$", REG_EXTENDED|REG_NOSUB), 0);
	big[255] = '\0';
	check_int("large repetition match", regexec(&r, big, 0, NULL, 0), 0);
	big[255] = 'a';
	big[256] = '\0';
	check_int("large repetition too long", regexec(&r, big, 0, NULL, 0),
	    REG_NOMATCH);
	regfree(&r);
	check_regex("(ab|cd){2,100}x", "zabcdabx", 0, 0, 0, "1-8:abcdabx");
	check_regex("(ab|cd){2,100}x", "zabcdabx", 0, 0, 1, "5-7:ab");
}

static void
check_width(const char *name, uint32_t cp, int expected)
{
	char	buf[64];

	snprintf(buf, sizeof buf, "wcwidth %s U+%04X", name, cp);
	check_int(buf, wcwidth(cp), expected);
}

static void
test_unicode(void)
{
	uint32_t	cp;
	char		buf[8];

	check_int("decode emoji", win32_utf8_decode("\xf0\x9f\x98\x80", 4,
	    &cp), 4);
	check_int("decode emoji value", cp, 0x1f600);
	check_int("decode overlong", win32_utf8_decode("\xc0\xaf", 2, &cp), -1);
	check_int("decode surrogate", win32_utf8_decode("\xed\xa0\x80", 3, &cp),
	    -1);
	check_int("decode too big", win32_utf8_decode("\xf4\x90\x80\x80", 4,
	    &cp), -1);
	check_int("decode truncated", win32_utf8_decode("\xe2\x82", 2, &cp),
	    -1);
	check_int("encode emoji", win32_utf8_encode(0x1f600, buf), 4);
	check_int("encode emoji bytes", memcmp(buf, "\xf0\x9f\x98\x80", 4), 0);
	check_int("encode surrogate", win32_utf8_encode(0xd800, buf), -1);

	check_width("ascii", 'a', 1);
	check_width("control", 0x1b, -1);
	check_width("hangul", 0xd55c, 2);
	check_width("hangul jamo initial", 0x1112, 2);
	check_width("hangul jamo medial", 0x1161, 0);
	check_width("hangul jamo final", 0x11ab, 0);
	check_width("hangul jamo ext-b", 0xd7b0, 0);
	check_width("hangul jamo ext-a", 0xa960, 2);
	check_width("hangul tone mark", 0x302e, 0);
	check_width("ideographic iteration", 0x302a, 0);
	check_width("kana voiced mark", 0x3099, 0);
	check_width("combining acute", 0x0301, 0);
	check_width("zero width space", 0x200b, 0);
	check_width("zero width joiner", 0x200d, 0);
	check_width("variation selector 16", 0xfe0f, 0);
	check_width("grinning face", 0x1f600, 2);
	check_width("check mark", 0x2705, 2);
	check_width("cross mark", 0x274c, 2);
	check_width("star", 0x2b50, 2);
	check_width("high voltage", 0x26a1, 2);
	check_width("hourglass", 0x231b, 2);
	check_width("cjk", 0x4e00, 2);
	check_width("cjk ext b", 0x20000, 2);
	check_width("fullwidth a", 0xff21, 2);
	check_width("nerd font", 0xf31b, 1);
	check_width("nerd font 2", 0xf385, 1);
	check_width("private use plane 15", 0xf0000, 1);
	check_width("e acute", 0xe9, 1);
	check_width("box drawing", 0x2500, 1);

	check_int("towupper", win32_towupper(0xe9), 0xc9);
	check_int("towlower", win32_towlower('A'), 'a');
	check_int("iswalpha hangul", win32_iswalpha(0xd55c), 1);
	check_int("iswalpha digit", win32_iswalpha('1'), 0);
}

int
main(void)
{
	test_time();
	test_fnmatch();
	test_regex();
	test_unicode();

	if (failures != 0) {
		fprintf(stderr, "%d failures\n", failures);
		return (1);
	}
	puts("Windows text unit tests passed");
	return (0);
}
