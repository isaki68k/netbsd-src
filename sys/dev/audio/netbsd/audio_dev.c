/*
 * NetBSD userland device.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <pthread.h>
// audioio.h 中の SLINEAR_NE とかを define させるため一時的に _KERNEL をつける
#define _KERNEL
#include "../../sys/audioio.h"
#undef _KERNEL
#include "audiovar.h"

extern const char *devicefile;
extern int hw_chan;
extern int hw_freq;

struct audio_softc local_sc;

struct userland_softc
{
	struct audio_softc *asc;	/* back link */

	int fd;
	int frame_bytes;
	int sent_count;
	kmutex_t sc_lock;
	kmutex_t sc_intr_lock;
	struct timeval tv;
};

int userland_start_output(void *, void *, int, void(*)(void *), void *);

void *
userland_allocm(void *hdl, int direction, size_t size)
{
	return malloc(size);
}

void
userland_freem(void *hdl, void *addr, size_t size)
{
	free(addr);
}

int
userland_halt_output(void *hdl)
{
	return 0;
}

struct audio_hw_if userland_hw_if = {
	.start_output = userland_start_output,
	.halt_output = userland_halt_output,
	.allocm = userland_allocm,
	.freem = userland_freem,
};

// hw が true なら OS のオーディオデバイスをオープンします。
void
audio_attach(struct audio_softc **scp, bool hw)
{
	struct audio_softc *sc;
	struct userland_softc *usc;
	struct audio_info ai;
	audio_format2_t phwfmt;
	audio_format2_t rhwfmt;
	int r;

	sc = &local_sc;
	*scp = sc;
	usc = calloc(1, sizeof(*usc));
	sc->hw_hdl = usc;

	usc->asc = sc;
	usc->fd = -1;

	phwfmt.encoding = AUDIO_ENCODING_SLINEAR_LE;
	phwfmt.channels = hw_chan;
	phwfmt.sample_rate = hw_freq;
	phwfmt.precision = 16;
	phwfmt.stride = 16;
	rhwfmt = phwfmt;
	usc->frame_bytes = phwfmt.precision / 8 * phwfmt.channels;

	sc->hw_if = &userland_hw_if;
	sc->sc_lock = &usc->sc_lock;
	sc->sc_intr_lock = &usc->sc_intr_lock;

	audio_softc_init(&phwfmt, &rhwfmt);

	if (hw) {
		usc->fd = open(devicefile, O_RDWR);
		if (usc->fd == -1) {
			printf("open failed: %s\n", devicefile);
			exit(1);
		}

		AUDIO_INITINFO(&ai);
		ai.mode = AUMODE_PLAY;
		ai.play.sample_rate = phwfmt.sample_rate;
		ai.play.encoding    = phwfmt.encoding;
		ai.play.precision   = phwfmt.precision;
		ai.play.channels    = phwfmt.channels;
		r = ioctl(usc->fd, AUDIO_SETINFO, &ai);
		if (r == -1) {
			printf("AUDIO_SETINFO failed\n");
			exit(1);
		}
	}
}

void
audio_detach(struct audio_softc *sc)
{
	struct userland_softc *usc = sc->hw_hdl;

	if (usc->fd != -1) {
		close(usc->fd);
		usc->fd = -1;
	}
	free(usc);
	sc->hw_hdl = NULL;
}

int
userland_start_output(void *hdl, void *blk, int blksize, void(*intr)(void *), void *arg)
{
	struct userland_softc *usc = hdl;

	int r = write(usc->fd, blk, blksize);
	if (r == -1) {
		printf("write failed: %s\n", strerror(errno));
		exit(1);
	}
	usc->sent_count += blksize / usc->frame_bytes;

	// ユーザランドプログラムの場合、
	// ハードウェアへの転送が終わったことに相当するのでここで直接
	// 割り込みハンドラを呼び出す。あっちに加工がしてある。
	intr(arg);

	return 0;
}
