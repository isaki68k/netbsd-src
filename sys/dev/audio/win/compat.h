#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define LITTLE_ENDIAN 1
#define BIG_ENDIAN 2
#define BYTE_ORDER LITTLE_ENDIAN


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

inline void
panic()
{
	exit(1);
}