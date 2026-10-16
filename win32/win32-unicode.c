/*
 * Unicode support for the Windows port: UTF-8 decoding and encoding,
 * display widths and character classes for full 32-bit code points.
 *
 * MSVC's wchar_t is only 16 bits, so the UCRT mbtowc(), wctomb(),
 * wcwidth() and isw*() functions cannot represent characters outside the
 * Basic Multilingual Plane (every emoji becomes U+FFFD). tmux, its regex
 * engine and fnmatch() use these functions instead.
 *
 * The width tables in win32-wcwidth.h are generated from the Unicode
 * Character Database by gen-wcwidth.py.
 *
 * The character class and case mapping tables (musl-alpha.h, musl-punct.h
 * and musl-casemap.h) and the functions using them are from musl libc
 * (src/ctype), which is licensed as follows:
 *
 * Copyright (c) 2005-2020 Rich Felker, et al.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "win32-unicode.h"

/*
 * Decode one UTF-8 character. Returns the number of bytes used, 0 if s
 * starts with a NUL, or -1 if the sequence is invalid or incomplete within
 * n bytes. Overlong forms, surrogates and values above U+10FFFF are
 * invalid.
 */
int
win32_utf8_decode(const char *s, size_t n, uint32_t *cp)
{
	const unsigned char	*u = (const unsigned char *)s;
	uint32_t		 c, min;
	size_t			 len, i;

	if (n == 0)
		return (-1);
	c = u[0];
	if (c == 0) {
		*cp = 0;
		return (0);
	}
	if (c < 0x80) {
		*cp = c;
		return (1);
	}
	if (c >= 0xc2 && c <= 0xdf) {
		len = 2;
		c &= 0x1f;
		min = 0x80;
	} else if (c >= 0xe0 && c <= 0xef) {
		len = 3;
		c &= 0x0f;
		min = 0x800;
	} else if (c >= 0xf0 && c <= 0xf4) {
		len = 4;
		c &= 0x07;
		min = 0x10000;
	} else
		return (-1);
	if (n < len)
		return (-1);
	for (i = 1; i < len; i++) {
		if ((u[i] & 0xc0) != 0x80)
			return (-1);
		c = (c << 6) | (u[i] & 0x3f);
	}
	if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
		return (-1);
	*cp = c;
	return ((int)len);
}

/*
 * Decode one character, never failing: an invalid byte is returned as a
 * code point in U+DC80-U+DCFF (like Python's surrogateescape) so that it
 * still counts as exactly one character but can never equal a valid one.
 * Returns 0 at a NUL byte or when n is 0.
 */
int
win32_utf8_decode_lax(const char *s, size_t n, uint32_t *cp)
{
	int	len;

	if (n == 0 || *s == '\0') {
		*cp = 0;
		return (0);
	}
	len = win32_utf8_decode(s, n, cp);
	if (len > 0)
		return (len);
	*cp = WIN32_UTF8_ESCAPE((unsigned char)*s);
	return (1);
}

/*
 * Encode a code point as UTF-8 into s (at least 4 bytes). Returns the
 * number of bytes or -1 for a surrogate or a value above U+10FFFF.
 */
int
win32_utf8_encode(uint32_t cp, char *s)
{
	unsigned char	*u = (unsigned char *)s;

	if (cp < 0x80) {
		u[0] = cp;
		return (1);
	}
	if (cp < 0x800) {
		u[0] = 0xc0 | (cp >> 6);
		u[1] = 0x80 | (cp & 0x3f);
		return (2);
	}
	if (cp >= 0xd800 && cp <= 0xdfff)
		return (-1);
	if (cp < 0x10000) {
		u[0] = 0xe0 | (cp >> 12);
		u[1] = 0x80 | ((cp >> 6) & 0x3f);
		u[2] = 0x80 | (cp & 0x3f);
		return (3);
	}
	if (cp > 0x10ffff)
		return (-1);
	u[0] = 0xf0 | (cp >> 18);
	u[1] = 0x80 | ((cp >> 12) & 0x3f);
	u[2] = 0x80 | ((cp >> 6) & 0x3f);
	u[3] = 0x80 | (cp & 0x3f);
	return (4);
}

struct win32_wcwidth_range {
	uint32_t	first;
	uint32_t	last;
};
#include "win32-wcwidth.h"

static int
win32_wcwidth_search(const struct win32_wcwidth_range *table, size_t n,
    uint32_t cp)
{
	size_t	lo = 0, hi = n, mid;

	if (n == 0 || cp < table[0].first || cp > table[n - 1].last)
		return (0);
	while (lo < hi) {
		mid = lo + (hi - lo) / 2;
		if (cp > table[mid].last)
			lo = mid + 1;
		else if (cp < table[mid].first)
			hi = mid;
		else
			return (1);
	}
	return (0);
}

/*
 * Return the display width of a code point: -1 for control characters
 * and invalid code points, 0 for combining and other zero width
 * characters, 2 for East Asian wide and fullwidth characters (including
 * emoji with emoji presentation) and 1 otherwise.
 */
int
win32_wcwidth(uint32_t cp)
{
	if (cp == 0)
		return (0);
	if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0))
		return (-1);
	if (cp < 0x300)
		return (1);
	if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
	    (cp & 0xfffe) == 0xfffe)
		return (-1);
	if (win32_wcwidth_search(win32_wcwidth_zero,
	    sizeof win32_wcwidth_zero / sizeof win32_wcwidth_zero[0], cp))
		return (0);
	if (win32_wcwidth_search(win32_wcwidth_wide,
	    sizeof win32_wcwidth_wide / sizeof win32_wcwidth_wide[0], cp))
		return (2);
	return (1);
}

/* Character classes, from musl. */

static const unsigned char win32_alpha_table[] = {
#include "musl-alpha.h"
};

static const unsigned char win32_punct_table[] = {
#include "musl-punct.h"
};

#include "musl-casemap.h"

static uint32_t
win32_casemap(uint32_t c, int dir)
{
	static const int	mt[] = { 2048, 342, 57 };
	unsigned		b, x, y, v, rt, xb, xn, try;
	int			r, rd, c0 = (int)c;

	if (c >= 0x20000)
		return (c);
	b = c >> 8;
	c &= 255;
	x = c / 3;
	y = c % 3;

	/* Look up entry in two-level base-6 table. */
	v = tab[tab[b] * 86 + x];
	v = (v * mt[y] >> 11) % 6;

	/*
	 * Use the bit vector out of the tables as an index into a
	 * block-specific set of rules and decode the rule into a type and a
	 * case-mapping delta.
	 */
	r = rules[rulebases[b] + v];
	rt = r & 255;
	rd = r >> 8;

	/*
	 * Rules 0/1 are simple lower/upper case with a delta. Apply according
	 * to desired mapping direction.
	 */
	if (rt < 2)
		return (c0 + (rd & -(int)(rt ^ dir)));

	/*
	 * Binary search. Endpoints of the binary search for this block are
	 * stored in the rule delta field.
	 */
	xn = rd & 0xff;
	xb = (unsigned)rd >> 8;
	while (xn) {
		try = exceptions[xb + xn / 2][0];
		if (try == c) {
			r = rules[exceptions[xb + xn / 2][1]];
			rt = r & 255;
			rd = r >> 8;
			if (rt < 2)
				return (c0 + (rd & -(int)(rt ^ dir)));
			/* Hard-coded for the four exceptional titlecase. */
			return (c0 + (dir ? -1 : 1));
		} else if (try > c)
			xn /= 2;
		else {
			xb += xn / 2;
			xn -= xn / 2;
		}
	}
	return (c0);
}

uint32_t
win32_towlower(uint32_t c)
{
	return (win32_casemap(c, 0));
}

uint32_t
win32_towupper(uint32_t c)
{
	return (win32_casemap(c, 1));
}

int
win32_iswalpha(uint32_t c)
{
	if (c < 0x20000U) {
		return ((win32_alpha_table[win32_alpha_table[c >> 8] * 32 +
		    ((c & 255) >> 3)] >> (c & 7)) & 1);
	}
	if (c < 0x2fffeU)
		return (1);
	return (0);
}

int
win32_iswpunct(uint32_t c)
{
	if (c < 0x20000U) {
		return ((win32_punct_table[win32_punct_table[c >> 8] * 32 +
		    ((c & 255) >> 3)] >> (c & 7)) & 1);
	}
	return (0);
}

int
win32_iswdigit(uint32_t c)
{
	return (c - '0' < 10);
}

int
win32_iswxdigit(uint32_t c)
{
	return (c - '0' < 10 || (c | 32) - 'a' < 6);
}

int
win32_iswalnum(uint32_t c)
{
	return (win32_iswdigit(c) || win32_iswalpha(c));
}

int
win32_iswblank(uint32_t c)
{
	return (c == ' ' || c == '\t');
}

int
win32_iswcntrl(uint32_t c)
{
	return (c < 32 ||
	    c - 0x7f < 33 ||
	    c - 0x2028 < 2 ||
	    c - 0xfff9 < 3);
}

/*
 * The Unicode White_Space property, minus non-breaking spaces (U+00A0,
 * U+2007 and U+202F) and script-specific characters with non-blank glyphs
 * (U+1680 and U+180E).
 */
int
win32_iswspace(uint32_t c)
{
	static const uint32_t	spaces[] = {
		' ', '\t', '\n', '\r', 11, 12, 0x0085,
		0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005,
		0x2006, 0x2008, 0x2009, 0x200a,
		0x2028, 0x2029, 0x205f, 0x3000
	};
	size_t			i;

	for (i = 0; i < sizeof spaces / sizeof spaces[0]; i++) {
		if (spaces[i] == c)
			return (1);
	}
	return (0);
}

/*
 * Consider all legal code points as printable except for C0 and C1
 * control characters, U+2028 and U+2029 (line/paragraph break) and
 * U+FFF9 through U+FFFB (interlinear annotation controls).
 */
int
win32_iswprint(uint32_t c)
{
	if (c < 0xffU)
		return (((c + 1) & 0x7f) >= 0x21);
	if (c < 0x2028U || c - 0x202aU < 0xd800 - 0x202a ||
	    c - 0xe000U < 0xfff9 - 0xe000)
		return (1);
	if (c - 0xfffcU > 0x10ffff - 0xfffc || (c & 0xfffe) == 0xfffe)
		return (0);
	return (1);
}

int
win32_iswgraph(uint32_t c)
{
	return (!win32_iswspace(c) && win32_iswprint(c));
}

int
win32_iswupper(uint32_t c)
{
	return (win32_towlower(c) != c);
}

int
win32_iswlower(uint32_t c)
{
	return (win32_towupper(c) != c);
}

/* Look up a character class by name. Returns 0 if it is unknown. */
int
win32_wctype(const char *name)
{
	static const char	*names[] = {
		"alnum", "alpha", "blank", "cntrl", "digit", "graph",
		"lower", "print", "punct", "space", "upper", "xdigit"
	};
	size_t			 i;

	for (i = 0; i < sizeof names / sizeof names[0]; i++) {
		if (strcmp(name, names[i]) == 0)
			return ((int)i + 1);
	}
	return (0);
}

/* Is a character in a class returned by win32_wctype()? */
int
win32_iswctype(uint32_t c, int type)
{
	switch (type) {
	case WIN32_WCTYPE_ALNUM:
		return (win32_iswalnum(c));
	case WIN32_WCTYPE_ALPHA:
		return (win32_iswalpha(c));
	case WIN32_WCTYPE_BLANK:
		return (win32_iswblank(c));
	case WIN32_WCTYPE_CNTRL:
		return (win32_iswcntrl(c));
	case WIN32_WCTYPE_DIGIT:
		return (win32_iswdigit(c));
	case WIN32_WCTYPE_GRAPH:
		return (win32_iswgraph(c));
	case WIN32_WCTYPE_LOWER:
		return (win32_iswlower(c));
	case WIN32_WCTYPE_PRINT:
		return (win32_iswprint(c));
	case WIN32_WCTYPE_PUNCT:
		return (win32_iswpunct(c));
	case WIN32_WCTYPE_SPACE:
		return (win32_iswspace(c));
	case WIN32_WCTYPE_UPPER:
		return (win32_iswupper(c));
	case WIN32_WCTYPE_XDIGIT:
		return (win32_iswxdigit(c));
	}
	return (0);
}
