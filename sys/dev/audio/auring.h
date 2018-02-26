#pragma once

#if defined(_KERNEL)
#include <dev/audio/audiovar.h>
#include <dev/audio/auformat.h>
#else
#include <memory.h>
#include "audiovar.h"
#include "auformat.h"
#endif

static inline bool
audio_ring_is_valid(const audio_ring_t *ring)
{
	KASSERT(ring != NULL);

	if (!is_valid_format(&ring->fmt)) {
		printf("%s: is_valid_format() failed\n", __func__);
		return false;
	}
	if (ring->capacity < 0) {
		printf("%s: capacity(%d) < 0\n", __func__, ring->capacity);
		return false;
	}
	if (ring->capacity > INT_MAX / 2) {
		printf("%s: capacity(%d) > INT_MAX/2\n", __func__,
		    ring->capacity);
		return false;
	}
	if (ring->count < 0) {
		printf("%s: count(%d) < 0\n", __func__, ring->count);
		return false;
	}
	if (ring->count > ring->capacity) {
		printf("%s: count(%d) < capacity(%d)\n", __func__,
		    ring->count, ring->capacity);
		return false;
	}
	if (ring->capacity == 0) {
		if (ring->mem != NULL) {
			printf("%s: capacity == 0 but mem != NULL\n",
			    __func__);
			return false;
		}
	} else {
		if (ring->mem == NULL) {
			printf("%s: capacity != 0 but mem == NULL\n",
			    __func__);
			return false;
		}
		if (ring->top < 0) {
			printf("%s: top(%d) < 0\n", __func__, ring->top);
			return false;
		}
		if (ring->top >= ring->capacity) {
			printf("%s: top(%d) >= capacity(%d)\n", __func__,
			    ring->top, ring->capacity);
			return false;
		}
	}
	return true;
}

// idx をラウンディングします。
// 加算方向で、加算量が ring->capacity 以下のケースのみサポートします。
static inline int
audio_ring_round(const audio_ring_t *ring, int idx)
{
	KASSERT(audio_ring_is_valid(ring));
	KASSERT(idx >= 0);
	KASSERT(idx < ring->capacity * 2);

	return idx >= ring->capacity ? idx - ring->capacity : idx;
}

// ring の bottom 位置(top+count位置) を返します。
// この位置は、最終有効フレームの次のフレーム位置に相当します。
static inline int
audio_ring_bottom(const audio_ring_t *ring)
{
	return audio_ring_round(ring, ring->top + ring->count);
}

// ring の top フレームのポインタを求めます。
static inline aint_t *
RING_TOP(const audio_ring_t *ring)
{
	return (aint_t *)ring->mem + ring->top * ring->fmt.channels;
}

// ring の bottom (= top + count、すなわち、最終有効フレームの次) フレームの
// ポインタを求めます。
// hwbuf のポインタはこちらではなく RING_BOT_UINT8() で取得してください。
static inline aint_t *
RING_BOT(const audio_ring_t *ring)
{
	return (aint_t *)ring->mem +
	    audio_ring_bottom(ring) * ring->fmt.channels;
}

// stride=24 用

// ring の top フレームのポインタを求めます。
static inline uint8_t *
RING_TOP_UINT8(const audio_ring_t *ring)
{
	return (uint8_t *)ring->mem +
	    ring->top * ring->fmt.channels * ring->fmt.stride / 8;
}

// ring の bottom (= top + count、すなわち、最終有効フレームの次) フレームの
// ポインタを求めます。HWbuf は 4bit/sample の可能性があるため RING_BOT() では
// なく必ずこちらを使用してください。
static inline uint8_t *
RING_BOT_UINT8(audio_ring_t *ring)
{
	return (uint8_t *)ring->mem +
	    audio_ring_bottom(ring) * ring->fmt.channels * ring->fmt.stride / 8;
}

// キャパシティをバイト単位で求めます。
static inline int
audio_ring_bytelen(const audio_ring_t *ring)
{
	// return frametobyte(ring, ring->capacity)
	return ring->capacity * ring->fmt.channels * ring->fmt.stride / 8;
}

// ring->top から n 個取り出したことにします。
static inline void
audio_ring_take(audio_ring_t *ring, int n)
{
	KASSERT(audio_ring_is_valid(ring));
	KASSERTMSG(n >= 0, "%s: n=%d", __func__, n);
	KASSERTMSG(ring->count >= n, "%s: ring->count=%d n=%d",
	    __func__, ring->count, n);

	ring->top = audio_ring_round(ring, ring->top + n);
	ring->count -= n;
}

// ring bottom に n 個付け足したことにします。
static inline void
audio_ring_push(audio_ring_t *ring, int n)
{
	KASSERT(audio_ring_is_valid(ring));
	KASSERT(n >= 0);
	KASSERTMSG(ring->count + n <= ring->capacity,
		"%s: ring->count=%d n=%d ring->capacity=%d",
		__func__, ring->count, n, ring->capacity);

	ring->count += n;
}

// ring->top の位置からの有効フレームにアクセスしようとするとき、
// ラウンディングせずにアクセス出来る個数を返します。
static inline int
audio_ring_get_contig_used(const audio_ring_t *ring)
{
	KASSERT(audio_ring_is_valid(ring));

	return ring->top + ring->count <= ring->capacity
	    ? ring->count : ring->capacity - ring->top;
}

// audio_ring_bottom の位置から空きフレームにアクセスしようとするとき、
// ラウンディングせずにアクセス出来る、空きフレームの個数を返します。
static inline int
audio_ring_get_contig_free(const audio_ring_t *ring)
{
	KASSERT(audio_ring_is_valid(ring));

	// ring の折り返し終端まで使用されているときは、
	// 開始位置はラウンディング後なので < が条件
	if (ring->top + ring->count < ring->capacity) {
		return ring->capacity - (ring->top + ring->count);
	} else {
		return ring->capacity - ring->count;
	}
}
