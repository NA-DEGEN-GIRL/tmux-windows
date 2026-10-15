/*
 * Unicode support for the Windows port (win32-unicode.c). All code points
 * are 32-bit because MSVC's wchar_t is only 16 bits.
 */

#ifndef WIN32_UNICODE_H
#define WIN32_UNICODE_H

#include <stddef.h>
#include <stdint.h>

/* Code point used for an invalid UTF-8 byte by win32_utf8_decode_lax(). */
#define WIN32_UTF8_ESCAPE(b) (0xdc00U | (uint32_t)(b))

int		win32_utf8_decode(const char *, size_t, uint32_t *);
int		win32_utf8_decode_lax(const char *, size_t, uint32_t *);
int		win32_utf8_encode(uint32_t, char *);
int		win32_wcwidth(uint32_t);

/* Character classes for win32_wctype() and win32_iswctype(). */
#define WIN32_WCTYPE_ALNUM 1
#define WIN32_WCTYPE_ALPHA 2
#define WIN32_WCTYPE_BLANK 3
#define WIN32_WCTYPE_CNTRL 4
#define WIN32_WCTYPE_DIGIT 5
#define WIN32_WCTYPE_GRAPH 6
#define WIN32_WCTYPE_LOWER 7
#define WIN32_WCTYPE_PRINT 8
#define WIN32_WCTYPE_PUNCT 9
#define WIN32_WCTYPE_SPACE 10
#define WIN32_WCTYPE_UPPER 11
#define WIN32_WCTYPE_XDIGIT 12

int		win32_wctype(const char *);
int		win32_iswctype(uint32_t, int);
int		win32_iswalnum(uint32_t);
int		win32_iswalpha(uint32_t);
int		win32_iswblank(uint32_t);
int		win32_iswcntrl(uint32_t);
int		win32_iswdigit(uint32_t);
int		win32_iswgraph(uint32_t);
int		win32_iswlower(uint32_t);
int		win32_iswprint(uint32_t);
int		win32_iswpunct(uint32_t);
int		win32_iswspace(uint32_t);
int		win32_iswupper(uint32_t);
int		win32_iswxdigit(uint32_t);
uint32_t	win32_towlower(uint32_t);
uint32_t	win32_towupper(uint32_t);

#endif /* WIN32_UNICODE_H */
