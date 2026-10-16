/*
 * regerror() for the Windows regular expression engine, from musl libc
 * (src/regex/regerror.c). See tre-regcomp.c and tre-regexec.c for the rest
 * of the engine.
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

#include <stdio.h>
#include <string.h>

#include "win32-regex.h"

/*
 * Error message strings for error codes listed in win32-regex.h. This list
 * needs to be in sync with the codes listed there, naturally.
 */
static const char messages[] = {
	"No error\0"
	"No match\0"
	"Invalid regexp\0"
	"Unknown collating element\0"
	"Unknown character class name\0"
	"Trailing backslash\0"
	"Invalid back reference\0"
	"Missing ']'\0"
	"Missing ')'\0"
	"Missing '}'\0"
	"Invalid contents of {}\0"
	"Invalid character range\0"
	"Out of memory\0"
	"Repetition not preceded by valid expression\0"
	"\0Unknown error"
};

size_t
regerror(int c, const regex_t *preg, char *buf, size_t size)
{
	const char	*s;
	unsigned	 e = c;

	(void)preg;
	for (s = messages; e && *s; e--, s += strlen(s) + 1)
		/* nothing */;
	if (!*s)
		s++;
	return (1 + snprintf(buf, size, "%s", s));
}
