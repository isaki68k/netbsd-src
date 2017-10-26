#pragma once
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/endian.h>

#define min(a,b)	(a < b ? a : b)
#define max(a,b)	(a > b ? a : b)

#define __noreturn __attribute__((__noreturn__))

typedef struct {
	uint16_t wFormatTag;
	uint16_t nChannels;
	uint32_t nSamplesPerSec;
	uint32_t nAvgBytesPerSec;
	uint16_t nBlockAlign;
	uint16_t wBitsPerSample;
	uint16_t cbSize;
} WAVEFORMATEX;

typedef struct {
	WAVEFORMATEX Format;
	union {
		uint16_t wValidBitsPerSample;
		uint16_t wSamplesPerBlock;
		uint16_t wReserved;
	};
	uint32_t dwChannelMask;
	// GUID SubFormat;
} WAVEFORMATEXTENSIBLE;

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
