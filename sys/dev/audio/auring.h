#pragma once

#ifdef DIAGNOSTIC
#define DIAGNOSTIC_ring(ring)	audio_diagnostic_ring(__func__, (ring))
#else
#define DIAGNOSTIC_ring(ring)
#endif

#ifdef DIAGNOSTIC
static void audio_diagnostic_ring(const char *, const audio_ring_t *);
static void
audio_diagnostic_ring(const char *func, const audio_ring_t *ring)
{

	KASSERTMSG(ring, "%s: ring == NULL", func);
	DIAGNOSTIC_format2(&ring->fmt);
	KASSERTMSG(0 <= ring->capacity && ring->capacity < INT_MAX / 2,
	    "%s: capacity(%d) is out of range", func, ring->capacity);
	KASSERTMSG(0 <= ring->used && ring->used <= ring->capacity,
	    "%s: used(%d) is out of range (capacity:%d)",
	    func, ring->used, ring->capacity);
	if (ring->capacity == 0) {
		KASSERTMSG(ring->mem == NULL,
		    "%s: capacity == 0 but mem != NULL", func);
	} else {
		KASSERTMSG(ring->mem != NULL,
		    "%s: capacity != 0 but mem == NULL", func);
		KASSERTMSG(0 <= ring->head && ring->head < ring->capacity,
		    "%s: head(%d) is out of range (capacity:%d)",
		    func, ring->head, ring->capacity);
	}
}
#endif

// idx をラウンディングします。
// 加算方向で、加算量が ring->capacity 以下のケースのみサポートします。
/*
 * Round idx.  idx must be non-negative and less than 2 * capacity.
 * This is a private function.
 */
static inline int
audio_ring_round(const audio_ring_t *ring, int idx)
{
	DIAGNOSTIC_ring(ring);
	KASSERT(idx >= 0);
	KASSERT(idx < ring->capacity * 2);

	if (idx < ring->capacity) {
		return idx;
	} else {
		return idx - ring->capacity;
	}
}

// ring の tail 位置(head+used位置) を返します。
// この位置は、最終有効フレームの次のフレーム位置に相当します。
/*
 * Return ring's tail (= head + used) position.
 */
static inline int
audio_ring_tail(const audio_ring_t *ring)
{
	return audio_ring_round(ring, ring->head + ring->used);
}

// ring の head フレームのポインタを求めます。
/*
 * Return ring's head pointer.
 * This function can be used only if the stride of the 'ring' is equal to
 * the internal stride.  Don't use this for hw buffer.
 */
static inline aint_t *
audio_ring_headptr_aint(const audio_ring_t *ring)
{
	KASSERT(ring->fmt.stride == sizeof(aint_t) * NBBY);

	return (aint_t *)ring->mem + ring->head * ring->fmt.channels;
}

// ring の tail (= head + used、すなわち、最終有効フレームの次) フレームの
// ポインタを求めます。
// hwbuf のポインタはこちらではなく RING_BOT_UINT8() で取得してください。
/*
 * Return ring's tail (= head + used) pointer.
 * This function can be used only if the stride of the 'ring' is equal to
 * the internal stride.  Don't use this for hw buffer.
 */
static inline aint_t *
audio_ring_tailptr_aint(const audio_ring_t *ring)
{
	KASSERT(ring->fmt.stride == sizeof(aint_t) * NBBY);

	return (aint_t *)ring->mem + audio_ring_tail(ring) * ring->fmt.channels;
}

// ring の head フレームのポインタを求めます。
/*
 * Return ring's head pointer.
 * This function can be used even if the stride of the 'ring' is equal to
 * or not equal to the internal stride.
 */
static inline uint8_t *
audio_ring_headptr(const audio_ring_t *ring)
{
	return (uint8_t *)ring->mem +
	    ring->head * ring->fmt.channels * ring->fmt.stride / NBBY;
}

// ring の tail (= head + used、すなわち、最終有効フレームの次) フレームの
// ポインタを求めます。HWbuf は 4bit/sample の可能性があるため RING_BOT() では
// なく必ずこちらを使用してください。
/*
 * Return ring's tail pointer.
 * This function can be used even if the stride of the 'ring' is equal to
 * or not equal to the internal stride.
 */
static inline uint8_t *
audio_ring_tailptr(audio_ring_t *ring)
{
	return (uint8_t *)ring->mem +
	    audio_ring_tail(ring) * ring->fmt.channels * ring->fmt.stride / NBBY;
}

// キャパシティをバイト単位で求めます。
/*
 * Return ring's capacity in bytes.
 */
static inline int
audio_ring_bytelen(const audio_ring_t *ring)
{
	// return frametobyte(ring, ring->capacity)
	return ring->capacity * ring->fmt.channels * ring->fmt.stride / NBBY;
}

// ring->head から n 個取り出したことにします。
/*
 * Take out n frames from head of ring.
 * This function only manipurates counters.  It doesn't manipurate any
 * actual buffer data.
 */
static inline void
audio_ring_take(audio_ring_t *ring, int n)
{
	DIAGNOSTIC_ring(ring);
	KASSERTMSG(n >= 0, "%s: n=%d", __func__, n);
	KASSERTMSG(ring->used >= n, "%s: ring->used=%d n=%d",
	    __func__, ring->used, n);

	ring->head = audio_ring_round(ring, ring->head + n);
	ring->used -= n;
}

// ring tail に n 個付け足したことにします。
/*
 * Append n frames into tail of ring.
 * This function only manipurates counters.  It doesn't manipurate any
 * actual buffer data.
 */
static inline void
audio_ring_push(audio_ring_t *ring, int n)
{
	DIAGNOSTIC_ring(ring);
	KASSERT(n >= 0);
	KASSERTMSG(ring->used + n <= ring->capacity,
		"%s: ring->used=%d n=%d ring->capacity=%d",
		__func__, ring->used, n, ring->capacity);

	ring->used += n;
}

// ring->head の位置からの有効フレームにアクセスしようとするとき、
// ラウンディングせずにアクセス出来る個数を返します。
/*
 * Return the number of contiguous frames in used.
 */
static inline int
audio_ring_get_contig_used(const audio_ring_t *ring)
{
	DIAGNOSTIC_ring(ring);

	if (ring->head + ring->used <= ring->capacity) {
		return ring->used;
	} else {
		return ring->capacity - ring->head;
	}
}

// audio_ring_tail の位置から空きフレームにアクセスしようとするとき、
// ラウンディングせずにアクセス出来る、空きフレームの個数を返します。
/*
 * Return the number of contiguous free frames.
 */
static inline int
audio_ring_get_contig_free(const audio_ring_t *ring)
{
	DIAGNOSTIC_ring(ring);

	// ring の折り返し終端まで使用されているときは、
	// 開始位置はラウンディング後なので < が条件
	if (ring->head + ring->used < ring->capacity) {
		return ring->capacity - (ring->head + ring->used);
	} else {
		return ring->capacity - ring->used;
	}
}
