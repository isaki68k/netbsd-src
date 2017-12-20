#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "queue.h"
// timeval
#include <winsock.h>
#include "aufilter.h"

#define LITTLE_ENDIAN 1
#define BIG_ENDIAN 2
#define BYTE_ORDER LITTLE_ENDIAN

#define __noreturn __declspec(noreturn)
#define __diagused

#define __arraycount(n) _countof(n)

typedef int64_t off_t;

inline
uint16_t __builtin_bswap16(uint16_t a)
{
	return (a << 8) | (a >> 8);
}

inline
uint32_t __builtin_bswap32(uint32_t a)
{
	return (a << 24) | ((a << 8) & 0x00ff0000) | ((a >> 8) & 0x0000ff00) | (a >> 24);
}

inline
uint16_t le16toh(uint16_t a)
{
#if BYTE_ORDER == LITTLE_ENDIAN
	return a;
#else
	return __builtin_bswap16(a);
#endif
}

inline
uint16_t be16toh(uint16_t a)
{
#if BYTE_ORDER == BIG_ENDIAN
	return a;
#else
	return __builtin_bswap16(a);
#endif
}

inline
uint32_t le32toh(uint32_t a)
{
#if BYTE_ORDER == LITTLE_ENDIAN
	return a;
#else
	return __builtin_bswap32(a);
#endif
}

inline
uint32_t be32toh(uint32_t a)
{
#if BYTE_ORDER == BIG_ENDIAN
	return a;
#else
	return __builtin_bswap32(a);
#endif
}

__noreturn
inline void
panic(const char *fmt, ...)
{
	exit(1);
}

static inline void
getmicrotime(struct timeval *tv)
{
	int64_t t = GetTickCount64();
	tv->tv_sec = (long)(t / 1000);
	tv->tv_usec = (long)((t % 1000) * 1000);
}

