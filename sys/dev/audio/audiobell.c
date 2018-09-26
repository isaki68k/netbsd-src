/*	$NetBSD: audiobell.c,v 1.25 2017/07/01 05:32:24 nat Exp $	*/

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

#define AUDIOBELL_RECTANGLE

#include <sys/types.h>
__KERNEL_RCSID(0, "$NetBSD: audiobell.c,v 1.25 2017/07/01 05:32:24 nat Exp $");

#include <sys/audioio.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/uio.h>

#include <dev/audio_if.h>
#include <dev/audiovar.h>
#include <dev/audiobellvar.h>

#include <dev/audio/audiodef.h>

#if defined(AUDIOBELL_RECTANGLE)
#include <dev/audiobelldata.h>
#endif

/* 44.1 kHz should reduce hum at higher pitches. */
#define BELL_SAMPLE_RATE	44100
#define BELL_SHIFT		3

#if defined(AUDIOBELL_RECTANGLE)

/* Rectanglar wave */
void
audiobell(void *v, u_int pitch, u_int period, u_int volume, int poll)
{
	dev_t audio;
	int16_t *buf;
	struct audiobell_arg bellarg;
	audio_file_t *file;
	struct uio auio;
	struct iovec aiov;
	int i;
	int remaincount;
	int remainlen;
	int wave1count;
	int wave1len;
	int16_t vol;

	KASSERT(volume <= 100);

	/* pitch limit 20 to Nyquist freq. */
	if (pitch > BELL_SAMPLE_RATE / 2)
		pitch = BELL_SAMPLE_RATE;
	if (pitch < 20)
		pitch = 20;

	buf = NULL;
	audio = AUDIO_DEVICE | device_unit((device_t)v);

	/* The audio system isn't built for polling. */
	if (poll)
		return;

	memset(&bellarg, 0, sizeof(bellarg));
	bellarg.sample_rate = BELL_SAMPLE_RATE;
	bellarg.encoding = AUDIO_ENCODING_SLINEAR_NE;
	bellarg.channels = 1;
	bellarg.precision = 16;

	/* If not configured, we can't beep. */
	if (audiobellopen(audio, &bellarg) != 0)
		return;

	file = bellarg.file;

	/* msec to sample count */
	remaincount = period * BELL_SAMPLE_RATE / 1000;
	remainlen = remaincount * sizeof(int16_t);

	/* generate single wave */
	wave1count = BELL_SAMPLE_RATE / pitch;
	wave1len = wave1count * sizeof(int16_t);

	buf = malloc(wave1len, M_TEMP, M_WAITOK);
	if (buf == NULL)
		goto out;

	vol = 32767 * volume / 100;
	for (i = 0; i < wave1count / 2; i++) {
		buf[i] = vol;
	}
	vol = -vol;
	for (; i < wave1count; i++) {
		buf[i] = vol;
	}

#define USE_PAUSE
#if defined(USE_PAUSE)
	/* pause */
	/* TODO: API */
	file->ptrack->is_pause = true;
#endif

	/* write to audio */
	for (; remainlen > 0; remainlen -= wave1len) {
		int len;
		len = uimin(remainlen, wave1len);
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

#if defined(USE_PAUSE)
		if (file->ptrack->usrbuf.used >= file->ptrack->usrbuf_blksize * NBLKHW) {
			file->ptrack->is_pause = false;
		}
#endif
	}
#if defined(USE_PAUSE)
	file->ptrack->is_pause = false;
#endif
out:
	if (buf != NULL)
		free(buf, M_TEMP);
	audiobellclose(file);
}

#else

static inline void
audiobell_expandwave(int16_t *buf)
{
	u_int i;

	for (i = 0; i < __arraycount(sinewave); i++)
		buf[i] = sinewave[i];
	for (i = __arraycount(sinewave); i < __arraycount(sinewave) * 2; i++)
		 buf[i] = buf[__arraycount(sinewave) * 2 - i - 1];
	for (i = __arraycount(sinewave) * 2; i < __arraycount(sinewave) * 4; i++)
		buf[i] = -buf[__arraycount(sinewave) * 4 - i - 1];
}

/*
 * The algorithm here is based on that described in the RISC OS Programmer's
 * Reference Manual (pp1624--1628).
 */
static inline int
audiobell_synthesize(int16_t *buf, u_int pitch, u_int period, u_int volume,
    uint16_t *phase)
{
	int16_t *wave;

	wave = malloc(sizeof(sinewave) * 4, M_TEMP, M_WAITOK);
	if (wave == NULL)
		return -1;
	audiobell_expandwave(wave);
	pitch = pitch * ((sizeof(sinewave) * 4) << BELL_SHIFT) /
	    BELL_SAMPLE_RATE / 2;
	period = period * BELL_SAMPLE_RATE / 1000 / 2;

	for (; period != 0; period--) {
		*buf++ = wave[*phase >> BELL_SHIFT];
		*phase += pitch;
	}

	free(wave, M_TEMP);
	return 0;
}

void
audiobell(void *v, u_int pitch, u_int period, u_int volume, int poll)
{
	dev_t audio;
	int16_t *buf;
	uint16_t phase;
	struct audiobell_arg bellarg;
	audio_file_t *file;
	struct uio auio;
	struct iovec aiov;
	int size, len;

	KASSERT(volume <= 100);

	buf = NULL;
	audio = AUDIO_DEVICE | device_unit((device_t)v);

	/* The audio system isn't built for polling. */
	if (poll)
		return;

	memset(&bellarg, 0, sizeof(bellarg));
	bellarg.sample_rate = BELL_SAMPLE_RATE;
	bellarg.encoding = AUDIO_ENCODING_SLINEAR_NE;
	bellarg.channels = 1;
	bellarg.precision = 16;

	/* If not configured, we can't beep. */
	if (audiobellopen(audio, &bellarg) != 0)
		return;

	file = bellarg.file;

	if (bellarg.blocksize < BELL_SAMPLE_RATE)
		bellarg.blocksize = BELL_SAMPLE_RATE;

	len = period * BELL_SAMPLE_RATE / 1000 * 2;
	size = uimin(len, bellarg.blocksize);
	if (size == 0)
		goto out;

	buf = malloc(size, M_TEMP, M_WAITOK);
	if (buf == NULL)
		goto out;

	phase = 0;
	while (len > 0) {
		size = uimin(len, bellarg.blocksize);
		if (audiobell_synthesize(buf, pitch, size *
				1000 / BELL_SAMPLE_RATE, volume, &phase) != 0)
			goto out;
		aiov.iov_base = (void *)buf;
		aiov.iov_len = size;
		auio.uio_iov = &aiov;
		auio.uio_iovcnt = 1;
		auio.uio_offset = 0;
		auio.uio_resid = size;
		auio.uio_rw = UIO_WRITE;
		UIO_SETUP_SYSSPACE(&auio);

		if (audiobellwrite(file, &auio) != 0)
			break;
		len -= size;
	}
out:
	if (buf != NULL)
		free(buf, M_TEMP);
	audiobellclose(file);
}
#endif
