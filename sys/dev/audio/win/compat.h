#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define LITTLE_ENDIAN 1
#define BIG_ENDIAN 2
#define BYTE_ORDER LITTLE_ENDIAN

#define __noreturn __declspec(noreturn)

#define __arraycount(n) _countof(n)

typedef int64_t off_t;
typedef void *kcondvar_t;

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

inline void
cv_init(kcondvar_t *cv, const char *msg)
{
	// nop
}

inline void
cv_destroy(kcondvar_t *cv)
{
	// nop
}
