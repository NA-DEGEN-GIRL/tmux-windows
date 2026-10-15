/*
 * Windows console TTY handling.
 * Replaces termios raw mode with Windows Console API.
 * Enables VT processing for terminal escape sequences.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32-platform.h"

static HANDLE hConsoleIn = INVALID_HANDLE_VALUE;
static HANDLE hConsoleOut = INVALID_HANDLE_VALUE;
static DWORD saved_in_mode = 0;
static DWORD saved_out_mode = 0;
static int mode_saved = 0;

static UINT saved_input_cp = 0;
static UINT saved_output_cp = 0;

/* Is this handle a console? */
int
win32_tty_is_console(HANDLE h)
{
	DWORD	mode;

	if (h == NULL || h == INVALID_HANDLE_VALUE)
		return (0);
	return (GetConsoleMode(h, &mode) != 0);
}

/*
 * Get the console to draw on: standard output if it is a console, otherwise
 * the console itself (when only standard input is a console, as on Unix the
 * terminal is the one on standard input).
 */
HANDLE
win32_tty_output(void)
{
	static HANDLE	conout = INVALID_HANDLE_VALUE;
	HANDLE		h;

	h = GetStdHandle(STD_OUTPUT_HANDLE);
	if (win32_tty_is_console(h))
		return (h);
	if (conout == INVALID_HANDLE_VALUE) {
		conout = CreateFileW(L"CONOUT$", GENERIC_READ|GENERIC_WRITE,
		    FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
		    NULL);
	}
	return (conout);
}

/*
 * Set console to raw VT mode.
 * Enables virtual terminal input and output processing.
 */
int
win32_tty_raw_mode(void)
{
	DWORD in_mode, out_mode;

	hConsoleIn = GetStdHandle(STD_INPUT_HANDLE);
	hConsoleOut = win32_tty_output();

	if (!win32_tty_is_console(hConsoleIn) ||
	    !win32_tty_is_console(hConsoleOut))
		return (-1);

	/* Save current modes, unless already raw. */
	if (!mode_saved) {
		GetConsoleMode(hConsoleIn, &saved_in_mode);
		GetConsoleMode(hConsoleOut, &saved_out_mode);
		mode_saved = 1;
	}

	/* Input: enable VT input, disable line input and echo. */
	in_mode = ENABLE_VIRTUAL_TERMINAL_INPUT;

	/* Output: enable VT processing and disable auto newlines. */
	out_mode = ENABLE_PROCESSED_OUTPUT |
	    ENABLE_VIRTUAL_TERMINAL_PROCESSING |
	    DISABLE_NEWLINE_AUTO_RETURN;

	SetConsoleMode(hConsoleIn, in_mode);
	SetConsoleMode(hConsoleOut, out_mode);

	return (0);
}

/*
 * Restore console mode.
 */
void
win32_tty_restore(void)
{
	if (!mode_saved)
		return;

	SetConsoleMode(hConsoleIn, saved_in_mode);
	SetConsoleMode(hConsoleOut, saved_out_mode);
	mode_saved = 0;
}

/*
 * Get console window size.
 */
int
win32_tty_get_size(int *cols, int *rows)
{
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	HANDLE h;

	h = win32_tty_output();
	if (h == INVALID_HANDLE_VALUE)
		return (-1);

	if (!GetConsoleScreenBufferInfo(h, &csbi))
		return (-1);

	*cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
	*rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
	return (0);
}

/* Put the console code pages back as they were before tmux started. */
void
win32_tty_restore_codepages(void)
{
	if (saved_output_cp != 0 && GetConsoleOutputCP() != saved_output_cp)
		SetConsoleOutputCP(saved_output_cp);
	if (saved_input_cp != 0 && GetConsoleCP() != saved_input_cp)
		SetConsoleCP(saved_input_cp);
}

/*
 * Initialize console for UTF-8. The original code pages are restored when the
 * process exits so the shell that started tmux is left as it was.
 */
void
win32_tty_init_utf8(void)
{
	UINT	in_cp, out_cp;

	in_cp = GetConsoleCP();
	out_cp = GetConsoleOutputCP();
	if (in_cp == 0 && out_cp == 0)
		return; /* no console */

	if (saved_input_cp == 0 && saved_output_cp == 0) {
		saved_input_cp = in_cp;
		saved_output_cp = out_cp;
		atexit(win32_tty_restore_codepages);
	}
	if (out_cp != CP_UTF8)
		SetConsoleOutputCP(CP_UTF8);
	if (in_cp != CP_UTF8)
		SetConsoleCP(CP_UTF8);
}
