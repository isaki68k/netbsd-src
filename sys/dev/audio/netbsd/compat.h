#pragma once
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/endian.h>
#include <sys/queue.h>
#include <sys/time.h>
#include "aufilter.h"

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

// Windows 側と同じになるんだが
typedef struct kcondvar kcondvar_t;
typedef struct kmutex kmutex_t;

struct kcondvar {
	volatile int v;
};

struct kmutex {
	volatile int v;
};

// Windows 側と同じになるんだが
struct audio_params {
	int sample_rate;
	int channels;
	int encoding;
	int validbits;
	int precision;
};
typedef struct audio_params audio_params_t;

// Windows 側と同じになるんだが
struct audio_hw_if {
	void *(*allocm)(void *, int, size_t);
	void (*freem)(void *, void *, size_t);

	int (*start_output)(void *, void *, int, void(*)(void *), void *);
	int (*trigger_output)(void *, void *, void *, int, void(*)(void *), void *, const audio_params_t *);

	int (*halt_output)(void *);
	int (*round_blocksize)(void *, int, int, const audio_params_t *);
	size_t (*round_buffersize)(void *, int, size_t);
};

// audiovar.h の前方参照
typedef struct audio_trackmixer audio_trackmixer_t;

// Windows 側と同じになるんだが
struct audio_softc
{
	SLIST_HEAD(files_head, audio_file) sc_files;		/* 開いているファイルのリスト */
	audio_trackmixer_t  *sc_pmixer;		/* 接続されている再生ミキサ */
	audio_trackmixer_t  *sc_rmixer;		/* 接続されている録音ミキサ */
	kmutex_t *sc_lock;
	kmutex_t *sc_intr_lock;
	struct audio_hw_if *hw_if;
	void *hw_hdl;
	int sc_eof;

	bool sc_pbusy;
	void *dev;
	audio_filter_reg_t sc_xxx_pfilreg;
	audio_filter_reg_t sc_xxx_rfilreg;
	audio_format2_t sc_pparams;
	audio_format2_t sc_rparams;

	void *phys; // 実物理デバイス

	kmutex_t sc_lock0;
	kmutex_t sc_intr_lock0;
	struct audio_hw_if hw_if0;
};

void audio_softc_init(struct audio_softc *sc);

#define panic(fmt...)	panic_func(__func__, fmt)

static inline void __noreturn
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

static inline void
cv_init(kcondvar_t *cv, const char *msg)
{
	cv->v = 0;
}

static inline void
cv_destroy(kcondvar_t *cv)
{
	// nop
}

static inline void
cv_broadcast(kcondvar_t *cv)
{
	cv->v = 1;
}

int cv_wait_sig(kcondvar_t *cv, void *lock);

static inline int
mutex_owned(kmutex_t *mutex)
{
	return (mutex->v != 0);
}

static inline void
mutex_enter(kmutex_t *mutex)
{
	mutex->v = 1;
}

static inline void
mutex_exit(kmutex_t *mutex)
{
	mutex->v = 0;
}

static inline int
mutex_tryenter(kmutex_t *mutex)
{
	if (mutex_owned(mutex)) {
		return 0;
	} else {
		mutex_enter(mutex);
		return 1;
	}
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
	gettimeofday(tv, NULL);
}

static inline void
aprint_error_dev(void *dev, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

static inline void
aprint_normal_dev(void *dev, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
