/*	$NetBSD: audiobell.c,v 1.2 2019/05/08 13:40:17 isaki Exp $	*/

/*
 * Copyright (c) 1999 Richard Earnshaw
 * Copyright (c) 2004 Ben Harris
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the RiscBSD team.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
__KERNEL_RCSID(0, "$NetBSD: audiobell.c,v 1.2 2019/05/08 13:40:17 isaki Exp $");

#include <sys/audioio.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/uio.h>

#include <dev/audio/audio_if.h>
#include <dev/audio/audiovar.h>
#include <dev/audio/audiodef.h>
#include <dev/audio/audiobellvar.h>

// 16角形あれば十分正弦波が近似できる。
// 原理的には1/4波あればいいのはわかっているがこのくらいなら1波用意したほうが
// コードが楽だし、データ量もしれてる。
// こちらは常にこの16点を出力するが再生周波数を変えることで任意の周波数を
// 再生している。
// audio 側が周波数変換で線形補間してくれるのでなめらかな波形になる。
// audiobell の周波数が高くなって(あるいはデバイスの周波数上限が低くて)
// 1波で16点表現できなくなる場合は、8点 -> 4点.. のように2^n で間引く。
// 最終的には2点になるので矩形波になる。
// 再生周波数は、ナイキストの定理から、デバイスのサンプリング周波数/2 が上限、
// それを超える音は上限値にする。どうせ出ないので。
// XXX 何dbか下げること
int16_t sinewave[] = {
	0,  12539,  23169,  30272,  32767,  30272,  23169,  12539,
	0, -12539, -23169, -30272, -32767, -30272, -23169, -12539,
};

/*
 * dev is a device_t for the audio device to use.
 * pitch is the pitch of the bell in Hz,
 * period is the length in ms,
 * volume is the amplitude in % of max,
 * poll is no longer used.
 */
void
audiobell(void *dev, u_int pitch, u_int period, u_int volume, int poll)
{
	dev_t audio;
	int16_t *buf;
	audio_file_t *file;
	audio_track_t *ptrack;
	struct uio auio;
	struct iovec aiov;
	int i;
	int j;
	int remaincount;
	int remainbytes;
	int wave1count;
	int wave1bytes;
	int blkbytes;
	int len;
	int step;
	int offset;
	int mixer_sample_rate;
	int sample_rate;

	KASSERT(volume <= 100);

	/* The audio system isn't built for polling. */
	if (poll)
		return;

	buf = NULL;
	audio = AUDIO_DEVICE | device_unit((device_t)dev);

	/* If not configured, we can't beep. */
	if (audiobellopen(audio, &file) != 0)
		return;

	ptrack = file->ptrack;
	mixer_sample_rate = ptrack->mixer->track_fmt.sample_rate;

	/* Limit pitch */
	if (pitch < 20)
		pitch = 20;

	offset = 0;
	if (pitch <= mixer_sample_rate / 16) {
		/* 16-point sine wave */
		step = 1;
	} else if (pitch <= mixer_sample_rate / 8) {
		/* 8-point sine wave */
		step = 2;
	} else if (pitch <= mixer_sample_rate / 4) {
		/* 4-point sine wave, aka, triangular wave */
		step = 4;
	} else {
		/* Rectangular wave */
		if (pitch > mixer_sample_rate / 2)
			pitch = mixer_sample_rate / 2;
		step = 8;
		offset = 4;
	}

	wave1count = __arraycount(sinewave) / step;
	sample_rate = pitch * wave1count;
	audiobellsetrate(file, sample_rate);

	/* msec to sample count. */
	remaincount = period * sample_rate / 1000;
	// 波形を1波出すための roundup
	remaincount = roundup(remaincount, wave1count);
	remainbytes = remaincount * sizeof(int16_t);
	wave1bytes = wave1count * sizeof(int16_t);

	blkbytes = ptrack->usrbuf_blksize;
	blkbytes = rounddown(blkbytes, wave1bytes);
	blkbytes = uimin(blkbytes, remainbytes);
	buf = malloc(blkbytes, M_TEMP, M_WAITOK);
	if (buf == NULL)
		goto out;

	/* Generate sinewave with specified volume */
	j = offset;
	for (i = 0; i < blkbytes / sizeof(int16_t); i++) {
		/* XXX audio already has track volume feature though #if 0 */
		buf[i] = sinewave[j] * volume / 100;
		j += step;
		j %= __arraycount(sinewave);
	}

	/* Write while paused to avoid begin inserted silence. */
	ptrack->is_pause = true;
	for (; remainbytes > 0; remainbytes -= blkbytes) {
		len = uimin(remainbytes, blkbytes);
		aiov.iov_base = (void *)buf;
		aiov.iov_len = len;
		auio.uio_iov = &aiov;
		auio.uio_iovcnt = 1;
		auio.uio_offset = 0;
		auio.uio_resid = len;
		auio.uio_rw = UIO_WRITE;
		UIO_SETUP_SYSSPACE(&auio);
		if (audiobellwrite(file, &auio) != 0)
			goto out;

		if (ptrack->usrbuf.used >= ptrack->usrbuf_blksize * NBLKHW)
			ptrack->is_pause = false;
	}
	/* Here we go! */
	ptrack->is_pause = false;
out:
	if (buf != NULL)
		free(buf, M_TEMP);
	audiobellclose(file);
}
