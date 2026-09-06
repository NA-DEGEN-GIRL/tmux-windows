#include <stdint.h>
#include <stdio.h>
#include <string.h>

char *tparm(const char *, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t,
    intptr_t, intptr_t, intptr_t, intptr_t);

void
win32_signal_notify(int signo)
{
	(void)signo;
}

static int
check(const char *name, const char *actual, const char *expected)
{
	if (strcmp(actual, expected) == 0)
		return (0);
	fprintf(stderr, "%s: expected %s, got %s\n", name, expected, actual);
	return (1);
}

int
main(void)
{
	static const char link[] = "https://example.test/status";
	static const char id[] = "id=codex-status";
	int errors = 0;

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

	if (errors != 0)
		return (1);
	puts("Windows tparm tests passed");
	return (0);
}
