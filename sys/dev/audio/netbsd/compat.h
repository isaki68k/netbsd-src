#pragma once
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/audioio.h>
#include <sys/endian.h>
#include <sys/queue.h>
#include <sys/time.h>

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

extern int panic_msgout;

static inline void __noreturn
panic_func(const char *caller, const char *fmt, ...)
{
	va_list ap;

	if (panic_msgout) {
		printf("panic: %s: ", caller);
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
		printf("\n");
	}

	abort();
}

static inline void
getmicrotime(struct timeval *tv)
{
	gettimeofday(tv, NULL);
}

