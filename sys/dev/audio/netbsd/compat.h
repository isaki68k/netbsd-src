#pragma once
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/endian.h>

#define min(a,b)	(a < b ? a : b)
#define max(a,b)	(a > b ? a : b)

#define panic(fmt...)	panic_func(__func__, fmt)

static inline void
panic_func(const char *caller, const char *fmt, ...)
{
	va_list ap;

	printf("panic: %s: ", caller);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");

	abort();
}
