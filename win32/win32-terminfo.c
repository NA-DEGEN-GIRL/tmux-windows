/*
 * Built-in terminal capabilities for Windows, replacing the ncurses terminfo
 * database.
 *
 * This is the ncurses xterm-256color description plus the extended
 * capabilities tmux uses for features that Windows Terminal and the
 * Windows 10+ console host implement: OSC 52 clipboard (Ms), cursor style
 * (Ss/Se) and colour (Cs/Cr), focus reporting, bracketed paste, RGB colour,
 * hyperlinks (Hls), underline styles and colours (Smulx/Setulc), overline,
 * strikethrough, synchronized output and the OSC 9;4 progress bar.
 * Terminals which don't support one of these ignore the sequence.
 *
 * DECSLRM margins and DECFRA are left out: a terminal which ignores them
 * (such as an older console host, or tmux itself when nested) would draw
 * the screen wrongly. They can be turned on with the margins and rectfill
 * terminal features. The terminal-features and terminal-overrides options
 * still apply on top of this table.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32-platform.h"

#define FLAG(n) { n, NULL, 0, 1, 3 }
#define NUM(n, v) { n, NULL, v, 0, 2 }
#define STR(n, v) { n, v, 0, 0, 1 }

static const struct win32_terminfo_entry win32_terminfo[] = {
	/* Boolean capabilities. */
	FLAG("am"),		/* auto_right_margin */
	FLAG("bce"),		/* back_color_erase */
	FLAG("AX"),		/* default colours (SGR 39/49) */
	FLAG("mir"),		/* move_insert_mode */
	FLAG("xenl"),		/* eat_newline_glitch */

	/* Numeric capabilities. */
	NUM("colors", 256),
	NUM("cols", 80),
	NUM("lines", 24),
	NUM("pairs", 32767),

	/* Cursor movement. */
	STR("cup", "\033[%i%p1%d;%p2%dH"),
	STR("cub1", "\010"),
	STR("cub", "\033[%p1%dD"),
	STR("cud1", "\012"),
	STR("cud", "\033[%p1%dB"),
	STR("cuf1", "\033[C"),
	STR("cuf", "\033[%p1%dC"),
	STR("cuu1", "\033[A"),
	STR("cuu", "\033[%p1%dA"),
	STR("home", "\033[H"),
	STR("hpa", "\033[%i%p1%dG"),
	STR("vpa", "\033[%i%p1%dd"),

	/* Scrolling. */
	STR("csr", "\033[%i%p1%d;%p2%dr"),
	STR("indn", "\033[%p1%dS"),
	STR("ind", "\012"),
	STR("ri", "\033M"),
	STR("rin", "\033[%p1%dT"),

	/* Editing. */
	STR("clear", "\033[H\033[2J"),
	STR("ed", "\033[J"),
	STR("el", "\033[K"),
	STR("el1", "\033[1K"),
	STR("ech", "\033[%p1%dX"),
	STR("dch1", "\033[P"),
	STR("dch", "\033[%p1%dP"),
	STR("dl1", "\033[M"),
	STR("dl", "\033[%p1%dM"),
	STR("ich", "\033[%p1%d@"),
	STR("il1", "\033[L"),
	STR("il", "\033[%p1%dL"),
	STR("E3", "\033[3J"),

	/* Attributes. */
	STR("bold", "\033[1m"),
	STR("dim", "\033[2m"),
	STR("sitm", "\033[3m"),
	STR("smul", "\033[4m"),
	STR("blink", "\033[5m"),
	STR("rev", "\033[7m"),
	STR("smso", "\033[7m"),
	STR("invis", "\033[8m"),
	STR("smxx", "\033[9m"),
	STR("Smol", "\033[53m"),
	STR("sgr0", "\033(B\033[m"),

	/* Underline styles and colours. */
	STR("Smulx", "\033[4:%p1%dm"),
	STR("Setulc", "\033[58:2:%p1%{65536}%/%d:%p1%{256}%/%{255}%&%d:"
	    "%p1%{255}%&%dm"),
	STR("Setulc1", "\033[58:5:%p1%dm"),
	STR("ol", "\033[59m"),

	/* Colours. */
	STR("setaf", "\033[%?%p1%{8}%<%t3%p1%d%e%p1%{16}%<%t9%p1%{8}%-%d"
	    "%e38;5;%p1%d%;m"),
	STR("setab", "\033[%?%p1%{8}%<%t4%p1%d%e%p1%{16}%<%t10%p1%{8}%-%d"
	    "%e48;5;%p1%d%;m"),
	STR("setrgbf", "\033[38;2;%p1%d;%p2%d;%p3%dm"),
	STR("setrgbb", "\033[48;2;%p1%d;%p2%d;%p3%dm"),
	STR("op", "\033[39;49m"),

	/* Cursor visibility, style (DECSCUSR) and colour (OSC 12). */
	STR("civis", "\033[?25l"),
	STR("cnorm", "\033[?12l\033[?25h"),
	STR("cvvis", "\033[?12;25h"),
	STR("Ss", "\033[%p1%d q"),
	STR("Se", "\033[0 q"),
	STR("Cs", "\033]12;%p1%s\007"),
	STR("Cr", "\033]112\007"),

	/* Alternate screen. */
	STR("smcup", "\033[?1049h\033[22;0;0t"),
	STR("rmcup", "\033[?1049l\033[23;0;0t"),

	/* Keypad. */
	STR("smkx", "\033[?1h\033="),
	STR("rmkx", "\033[?1l\033>"),

	/* Tabs, bell and carriage return. */
	STR("ht", "\011"),
	STR("tbc", "\033[3g"),
	STR("hts", "\033H"),
	STR("bel", "\007"),
	STR("cr", "\015"),

	/* Line drawing. */
	STR("enacs", ""),
	STR("smacs", "\033(0"),
	STR("rmacs", "\033(B"),
	STR("acsc", "``aaffggiijjkkllmmnnooppqqrrssttuuvvwwxxyyzz{{||}}~~"),

	/* Title. */
	STR("tsl", "\033]0;"),
	STR("fsl", "\007"),

	/* Clipboard (OSC 52) and hyperlinks (OSC 8). */
	STR("Ms", "\033]52;%p1%s;%p2%s\007"),
	STR("Hls", "\033]8;%?%p1%l%tid=%p1%s%;;%p2%s\033\\"),

	/* Bracketed paste, focus reporting and extended keys. */
	STR("Enbp", "\033[?2004h"),
	STR("Dsbp", "\033[?2004l"),
	STR("Enfcs", "\033[?1004h"),
	STR("Dsfcs", "\033[?1004l"),
	STR("Eneks", "\033[>4;2m"),
	STR("Dseks", "\033[>4m"),

	/* Synchronized output. */
	STR("Sync", "\033[?2026%?%p1%{1}%-%tl%eh%;"),

	/* Taskbar progress (OSC 9;4). */
	STR("Spb", "\033]9;4;%p1%d;%p2%d\033\\"),

	/* Keys. */
	STR("kmous", "\033[M"),
	STR("kcub1", "\033OD"),
	STR("kcuf1", "\033OC"),
	STR("kcuu1", "\033OA"),
	STR("kcud1", "\033OB"),
	STR("kbs", "\177"),
	STR("kdch1", "\033[3~"),
	STR("khome", "\033OH"),
	STR("kend", "\033OF"),
	STR("kpp", "\033[5~"),
	STR("knp", "\033[6~"),
	STR("kich1", "\033[2~"),
	STR("kcbt", "\033[Z"),
	STR("kind", "\033[1;2B"),
	STR("kri", "\033[1;2A"),

	/* Shift (2), Alt (3), Shift+Alt (4), Ctrl (5) and more. */
	STR("kDC", "\033[3;2~"),
	STR("kDC3", "\033[3;3~"),
	STR("kDC4", "\033[3;4~"),
	STR("kDC5", "\033[3;5~"),
	STR("kDC6", "\033[3;6~"),
	STR("kDC7", "\033[3;7~"),
	STR("kDN", "\033[1;2B"),
	STR("kDN3", "\033[1;3B"),
	STR("kDN4", "\033[1;4B"),
	STR("kDN5", "\033[1;5B"),
	STR("kDN6", "\033[1;6B"),
	STR("kDN7", "\033[1;7B"),
	STR("kEND", "\033[1;2F"),
	STR("kEND3", "\033[1;3F"),
	STR("kEND4", "\033[1;4F"),
	STR("kEND5", "\033[1;5F"),
	STR("kEND6", "\033[1;6F"),
	STR("kEND7", "\033[1;7F"),
	STR("kHOM", "\033[1;2H"),
	STR("kHOM3", "\033[1;3H"),
	STR("kHOM4", "\033[1;4H"),
	STR("kHOM5", "\033[1;5H"),
	STR("kHOM6", "\033[1;6H"),
	STR("kHOM7", "\033[1;7H"),
	STR("kIC", "\033[2;2~"),
	STR("kIC3", "\033[2;3~"),
	STR("kIC4", "\033[2;4~"),
	STR("kIC5", "\033[2;5~"),
	STR("kIC6", "\033[2;6~"),
	STR("kIC7", "\033[2;7~"),
	STR("kLFT", "\033[1;2D"),
	STR("kLFT3", "\033[1;3D"),
	STR("kLFT4", "\033[1;4D"),
	STR("kLFT5", "\033[1;5D"),
	STR("kLFT6", "\033[1;6D"),
	STR("kLFT7", "\033[1;7D"),
	STR("kNXT", "\033[6;2~"),
	STR("kNXT3", "\033[6;3~"),
	STR("kNXT4", "\033[6;4~"),
	STR("kNXT5", "\033[6;5~"),
	STR("kNXT6", "\033[6;6~"),
	STR("kNXT7", "\033[6;7~"),
	STR("kPRV", "\033[5;2~"),
	STR("kPRV3", "\033[5;3~"),
	STR("kPRV4", "\033[5;4~"),
	STR("kPRV5", "\033[5;5~"),
	STR("kPRV6", "\033[5;6~"),
	STR("kPRV7", "\033[5;7~"),
	STR("kRIT", "\033[1;2C"),
	STR("kRIT3", "\033[1;3C"),
	STR("kRIT4", "\033[1;4C"),
	STR("kRIT5", "\033[1;5C"),
	STR("kRIT6", "\033[1;6C"),
	STR("kRIT7", "\033[1;7C"),
	STR("kUP", "\033[1;2A"),
	STR("kUP3", "\033[1;3A"),
	STR("kUP4", "\033[1;4A"),
	STR("kUP5", "\033[1;5A"),
	STR("kUP6", "\033[1;6A"),
	STR("kUP7", "\033[1;7A"),

	/* Function keys: F1-F12, then with Shift, Ctrl, Ctrl+Shift, Alt. */
	STR("kf1", "\033OP"),
	STR("kf2", "\033OQ"),
	STR("kf3", "\033OR"),
	STR("kf4", "\033OS"),
	STR("kf5", "\033[15~"),
	STR("kf6", "\033[17~"),
	STR("kf7", "\033[18~"),
	STR("kf8", "\033[19~"),
	STR("kf9", "\033[20~"),
	STR("kf10", "\033[21~"),
	STR("kf11", "\033[23~"),
	STR("kf12", "\033[24~"),
	STR("kf13", "\033[1;2P"),
	STR("kf14", "\033[1;2Q"),
	STR("kf15", "\033[1;2R"),
	STR("kf16", "\033[1;2S"),
	STR("kf17", "\033[15;2~"),
	STR("kf18", "\033[17;2~"),
	STR("kf19", "\033[18;2~"),
	STR("kf20", "\033[19;2~"),
	STR("kf21", "\033[20;2~"),
	STR("kf22", "\033[21;2~"),
	STR("kf23", "\033[23;2~"),
	STR("kf24", "\033[24;2~"),
	STR("kf25", "\033[1;5P"),
	STR("kf26", "\033[1;5Q"),
	STR("kf27", "\033[1;5R"),
	STR("kf28", "\033[1;5S"),
	STR("kf29", "\033[15;5~"),
	STR("kf30", "\033[17;5~"),
	STR("kf31", "\033[18;5~"),
	STR("kf32", "\033[19;5~"),
	STR("kf33", "\033[20;5~"),
	STR("kf34", "\033[21;5~"),
	STR("kf35", "\033[23;5~"),
	STR("kf36", "\033[24;5~"),
	STR("kf37", "\033[1;6P"),
	STR("kf38", "\033[1;6Q"),
	STR("kf39", "\033[1;6R"),
	STR("kf40", "\033[1;6S"),
	STR("kf41", "\033[15;6~"),
	STR("kf42", "\033[17;6~"),
	STR("kf43", "\033[18;6~"),
	STR("kf44", "\033[19;6~"),
	STR("kf45", "\033[20;6~"),
	STR("kf46", "\033[21;6~"),
	STR("kf47", "\033[23;6~"),
	STR("kf48", "\033[24;6~"),
	STR("kf49", "\033[1;3P"),
	STR("kf50", "\033[1;3Q"),
	STR("kf51", "\033[1;3R"),
	STR("kf52", "\033[1;3S"),
	STR("kf53", "\033[15;3~"),
	STR("kf54", "\033[17;3~"),
	STR("kf55", "\033[18;3~"),
	STR("kf56", "\033[19;3~"),
	STR("kf57", "\033[20;3~"),
	STR("kf58", "\033[21;3~"),
	STR("kf59", "\033[23;3~"),
	STR("kf60", "\033[24;3~"),
	STR("kf61", "\033[1;4P"),
	STR("kf62", "\033[1;4Q"),
	STR("kf63", "\033[1;4R"),

	{ NULL, NULL, 0, 0, 0 }
};

/*
 * Look up a terminal capability by name.
 * Returns the entry or NULL.
 */
const struct win32_terminfo_entry *
win32_terminfo_find(const char *name)
{
	const struct win32_terminfo_entry *e;

	for (e = win32_terminfo; e->name != NULL; e++) {
		if (strcmp(e->name, name) == 0)
			return (e);
	}
	return (NULL);
}

/*
 * Get count of terminfo entries (excluding sentinel).
 */
u_int
win32_terminfo_count(void)
{
	u_int n = 0;
	const struct win32_terminfo_entry *e;

	for (e = win32_terminfo; e->name != NULL; e++)
		n++;
	return (n);
}

/*
 * Get all terminfo entries and optionally the count.
 */
const struct win32_terminfo_entry *
win32_terminfo_table(u_int *count)
{
	if (count != NULL)
		*count = win32_terminfo_count();
	return (win32_terminfo);
}
