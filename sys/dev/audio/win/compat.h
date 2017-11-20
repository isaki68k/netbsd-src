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

typedef struct kcondvar kcondvar_t;

struct kcondvar {
	volatile int v;
};

struct audio_params {
	int sample_rate;
	int channels;
	int encoding;
	int validbits;
	int precision;
};
typedef struct audio_params audio_params_t;

struct audio_hw_if {
	void *(*allocm)(void *, int, size_t);
	void (*freem)(void *, void *, size_t);

	int (*start_output)(void *, void *, int, void(*)(void *), void *);
	int (*trigger_output)(void *, void *, void *, int, void(*)(void *), void *, const audio_params_t *);

	int (*halt_output)(void *);
	audio_filter_t (*get_swcode)(void *, int, audio_filter_arg_t *);
	int (*round_blocksize)(void *, int, int, const audio_params_t *);
	size_t (*round_buffersize)(void *, int, size_t);
};

// audiovar.h の前方参照
typedef struct audio_trackmixer audio_trackmixer_t;

/* Userland から見えるデバイス */
struct audio_softc
{
	SLIST_HEAD(files_head, audio_file) sc_files;		/* 開いているファイルのリスト */
	audio_trackmixer_t  *sc_pmixer;		/* 接続されている再生ミキサ */
	audio_trackmixer_t  *sc_rmixer;		/* 接続されている録音ミキサ */
	void *sc_lock;
	void *sc_intr_lock;
	struct audio_hw_if *hw_if;
	void *hw_hdl;
	int sc_eof;

	bool sc_pbusy;
	void *dev;

	void *phys; // 実物理デバイス

	int sc_lock0;
	int sc_intr_lock0;
	struct audio_hw_if hw_if0;
};

void audio_softc_init(struct audio_softc *sc);

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
	cv->v = 0;
}

inline void
cv_destroy(kcondvar_t *cv)
{
	// nop
}

inline void
cv_broadcast(kcondvar_t *cv)
{
	cv->v = 1;
}

int cv_wait_sig(kcondvar_t *cv, void *lock);

static inline int
mutex_owned(void *mutex)
{
	return (*(int*)mutex != 0);
}

static inline void
mutex_enter(void *mutex)
{
	*(int*)mutex = 1;
}

static inline void
mutex_exit(void *mutex)
{
	*(int*)mutex = 0;
}

#define M_NOWAIT	(0)
static inline void *
kern_malloc(size_t size, int flags)
{
	return malloc(size);
}

static inline void *
kern_realloc(void *ptr, size_t size, int flags)
{
	return realloc(ptr, size);
}

static inline void
kern_free(void *ptr)
{
	free(ptr);
}

static inline void
getmicrotime(struct timeval *tv)
{
	int64_t t = GetTickCount64();
	tv->tv_sec = (long)(t / 1000);
	tv->tv_usec = (long)((t % 1000) * 1000);
}

void aprint_error_dev(void *, const char *fmt, ...);

void aprint_normal_dev(void *, const char *fmt, ...);
