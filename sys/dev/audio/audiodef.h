#ifndef _SYS_DEV_AUDIO_AUDIODEF_H_
#define _SYS_DEV_AUDIO_AUDIODEF_H_

// 出力バッファのブロック数
/* Number of output buffer's blocks.  Must be != NBLKHW */
#define NBLKOUT	(4)

// ハードウェアバッファのブロック数
/* Number of HW buffer's blocks. */
#define NBLKHW (3)

// ユーザバッファの最小ブロック数
/* Minimum number of usrbuf's blocks. */
#define AUMINNOBLK	(3)

// 1 ブロックの時間 [msec]
// 40ms の場合は (1/40ms) = 25 = 5^2 なので 100 の倍数の周波数のほか、
// 15.625kHz でもフレーム数が整数になるので、40 を基本にする。
#if !defined(AUDIO_BLK_MS)
#if defined(x68k)
// x68k では 40msec だと長い曲でアンダーランするので伸ばしておく
#define AUDIO_BLK_MS 320
#else
#define AUDIO_BLK_MS 40
#endif
#endif

// ミキサをシングルバッファにするかどうか。
// オン(シングルバッファ)にすると、レイテンシは1ブロック減らせるがマシン
// パワーがないと HW 再生が途切れる(かもしれない、速ければへーきへーき?)。
// オフ(ダブるバッファ)にすると、レイテンシは1ブロック増えるが HW 再生が
// 途切れることはなくなる。
//#define AUDIO_HW_SINGLE_BUFFER

// C の実装定義動作を使用する。
#define AUDIO_USE_C_IMPLEMENTATION_DEFINED_BEHAVIOR

static inline int
frametobyte(const audio_format2_t *fmt, int frames)
{
	return frames * fmt->channels * fmt->stride / NBBY;
}

// 周波数が fmt(.sample_rate) で表されるエンコーディングの
// 1ブロックのフレーム数を返します。
static inline int
frame_per_block(const audio_trackmixer_t *mixer, const audio_format2_t *fmt)
{
	return (fmt->sample_rate * mixer->blktime_n + mixer->blktime_d - 1) /
	    mixer->blktime_d;
}

#if defined(_KERNEL)
#include <dev/audio/auring.h>
#else
#include "auring.h"
#endif

#endif /* !_SYS_DEV_AUDIO_AUDIODEF_H_ */
