#ifndef _SYS_DEV_AUDIO_MLOG_H_
#define _SYS_DEV_AUDIO_MLOG_H_

// XXX とりあえずね

#include <sys/atomic.h>

// 本当はデバイスごとにすべきなのだが、そうすると今 sc を受け取っておらず
// DPRINTF() を呼んでる関数全員に sc を渡すようにしないといけなくなるので、
// それはちょっと面倒だし本題ではないので。

static void audio_mlog_init(void);
static void audio_mlog_free(void);
static void audio_mlog_flush(void);
static void audio_mlog_softintr(void *);
#if AUDIO_DEBUG >= 4
static void audio_mlog_printf(const char *, ...);
static void audio_mlog_vprintf(const char *, va_list);
#endif

static int mlog_refs;		// reference counter
static char *mlog_buf;
static int mlog_buflen;		// バッファ長
static int mlog_used;		// 使用中のバッファ文字列の長さ
static int mlog_full;		// バッファが一杯になってドロップした行数
static int mlog_drop;		// バッファ使用中につきドロップした行数
static volatile uint32_t mlog_inuse;	// 使用中フラグ
static void *mlog_sih;		// softint ハンドラ

static void
audio_mlog_init(void)
{
	mlog_refs++;
	if (mlog_refs > 1)
		return;
	mlog_buflen = 8192;
	mlog_buf = kmem_zalloc(mlog_buflen, KM_SLEEP);
	mlog_used = 0;
	mlog_full = 0;
	mlog_drop = 0;
	mlog_inuse = 0;
	mlog_sih = softint_establish(SOFTINT_SERIAL, audio_mlog_softintr, NULL);
	if (mlog_sih == NULL)
		printf("%s: softint_establish failed\n", __func__);
}

static void
audio_mlog_free(void)
{
	mlog_refs--;
	if (mlog_refs > 0)
		return;

	audio_mlog_flush();
	if (mlog_sih)
		softint_disestablish(mlog_sih);
	kmem_free(mlog_buf, mlog_buflen);
}

// 一時バッファの内容を出力します。
// ハードウェア割り込みコンテキスト以外で使用します。
static void
audio_mlog_flush(void)
{
	// すでに使用中なら何もしない?
	if (atomic_swap_32(&mlog_inuse, 1) == 1)
		return;

	if (mlog_used > 0) {
		printf("%s", mlog_buf);
		if (mlog_drop > 0)
			printf("mlog_drop %d\n", mlog_drop);
		if (mlog_full > 0)
			printf("mlog_full %d\n", mlog_full);
	}
	mlog_used = 0;
	mlog_full = 0;
	mlog_drop = 0;

	atomic_swap_32(&mlog_inuse, 0);
}

static void
audio_mlog_softintr(void *cookie)
{
	audio_mlog_flush();
}

#if AUDIO_DEBUG >= 4

// 一時バッファに書き込みます。
static void
audio_mlog_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	audio_mlog_vprintf(fmt, ap);
	va_end(ap);
}

// 一時バッファに書き込みます。
static void
audio_mlog_vprintf(const char *fmt, va_list ap)
{
	char buf[512];
	int len;

	len = vsnprintf(buf, sizeof(buf), fmt, ap);

	if (atomic_swap_32(&mlog_inuse, 1) == 1) {
		/* already inuse */
		mlog_drop++;
		return;
	}
	if (mlog_full == 0 && mlog_used + len < mlog_buflen) {
		strlcpy(mlog_buf + mlog_used, buf, mlog_buflen - mlog_used);
		mlog_used += len;
	} else {
		mlog_full++;
	}
	atomic_swap_32(&mlog_inuse, 0);

	if (mlog_sih)
		softint_schedule(mlog_sih);
}

#endif /* AUDIO_DEBUG >= 4 */

#endif /* _SYS_DEV_AUDIO_MLOG_H_ */
