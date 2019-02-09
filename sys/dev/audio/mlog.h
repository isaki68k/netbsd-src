#pragma once

/* XXX This is too sloppy in-memory logger. */

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
#else
#define audio_mlog_printf(fmt...) (void)0
#define audio_mlog_vprintf(fmt, va_list) (void)0
#endif

static int mlog_refs;		// reference counter
static char *mlog_buf[2];	// ダブルバッファ
static int mlog_buflen;		// バッファ長
static int mlog_used;		// 使用中のバッファ文字列の長さ
static int mlog_full;		// バッファが一杯になってドロップした行数
static int mlog_drop;		// バッファ使用中につきドロップした行数
static volatile uint32_t mlog_inuse;	// 使用中フラグ
static int mlog_wpage;		// 書き込みページ
static void *mlog_sih;		// softint ハンドラ

static void
audio_mlog_init(void)
{
	mlog_refs++;
	if (mlog_refs > 1)
		return;
	mlog_buflen = 4096;
	mlog_buf[0] = kmem_zalloc(mlog_buflen, KM_SLEEP);
	mlog_buf[1] = kmem_zalloc(mlog_buflen, KM_SLEEP);
	mlog_used = 0;
	mlog_full = 0;
	mlog_drop = 0;
	mlog_inuse = 0;
	mlog_wpage = 0;
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
	kmem_free(mlog_buf[0], mlog_buflen);
	kmem_free(mlog_buf[1], mlog_buflen);
}

// 一時バッファの内容を出力する。
// ハードウェア割り込みコンテキスト以外で使用する。
static void
audio_mlog_flush(void)
{
	if (mlog_refs == 0)
		return;

	// すでに使用中なら何もしない?
	if (atomic_swap_32(&mlog_inuse, 1) == 1)
		return;

	int rpage = mlog_wpage;
	mlog_wpage ^= 1;
	mlog_buf[mlog_wpage][0] = '\0';
	mlog_used = 0;

	// ロック解除
	atomic_swap_32(&mlog_inuse, 0);

	if (mlog_buf[rpage][0] != '\0') {
		printf("%s", mlog_buf[rpage]);
		if (mlog_drop > 0)
			printf("mlog_drop %d\n", mlog_drop);
		if (mlog_full > 0)
			printf("mlog_full %d\n", mlog_full);
	}
	mlog_full = 0;
	mlog_drop = 0;
}

static void
audio_mlog_softintr(void *cookie)
{
	audio_mlog_flush();
}

#if AUDIO_DEBUG >= 4

// 一時バッファに書き込む。
static void
audio_mlog_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	audio_mlog_vprintf(fmt, ap);
	va_end(ap);
}

// 一時バッファに書き込む。
static void
audio_mlog_vprintf(const char *fmt, va_list ap)
{
	int len;

	if (atomic_swap_32(&mlog_inuse, 1) == 1) {
		/* already inuse */
		mlog_drop++;
		return;
	}

	len = vsnprintf(
	    mlog_buf[mlog_wpage] + mlog_used,
	    mlog_buflen - mlog_used,
	    fmt,
	    ap);
	mlog_used += len;
	if (mlog_buflen - mlog_used <= 1) {
		mlog_full++;
	}

	atomic_swap_32(&mlog_inuse, 0);

	if (mlog_sih)
		softint_schedule(mlog_sih);
}

#endif /* AUDIO_DEBUG >= 4 */
