#include "aumix.h"
#include "auintr.h"
#include "auring.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <pthread.h>

extern const char *devicefile;

struct audio_dev_netbsd
{
	int fd;
	int frame_bytes;
	int sent_count;
	pthread_mutex_t mutex;
	struct timeval tv;
};
typedef struct audio_dev_netbsd audio_dev_netbsd_t;

int netbsd_start_output(void *, void *, int, void(*)(void *), void *);

void
lock(struct audio_softc *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;
	pthread_mutex_lock(&dev->mutex);
}

void
unlock(struct audio_softc *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;
	pthread_mutex_unlock(&dev->mutex);
}

void *
netbsd_allocm(void *hdl, int direction, size_t size)
{
	return malloc(size);
}

void
netbsd_freem(void *hdl, void *addr, size_t size)
{
	free(addr);
}

int
netbsd_halt_output(void *hdl)
{
	return 0;
}

// hw が true なら OS のオーディオデバイスをオープンします。
void
audio_attach(struct audio_softc **softc, bool hw)
{
	struct audio_softc *sc;
	audio_dev_netbsd_t *dev;
	struct audio_info ai;
	int r;

	sc = calloc(1, sizeof(*sc));
	*softc = sc;
	sc->phys = calloc(1, sizeof(*dev));

	dev = sc->phys;
	dev->fd = -1;

	sc->sc_phwfmt.encoding = AUDIO_ENCODING_SLINEAR_LE;
	sc->sc_phwfmt.channels = 2;
	sc->sc_phwfmt.sample_rate = 48000;
	sc->sc_phwfmt.precision = 16;
	sc->sc_phwfmt.stride = 16;
	dev->frame_bytes = sc->sc_phwfmt.precision / 8 * sc->sc_phwfmt.channels;

	pthread_mutex_init(&dev->mutex, NULL);

	audio_softc_init(sc);

	sc->hw_if->allocm = netbsd_allocm;
	sc->hw_if->freem = netbsd_freem;
	sc->hw_if->start_output = netbsd_start_output;
	sc->hw_if->halt_output = netbsd_halt_output;
	sc->hw_hdl = sc;

	if (hw) {
		dev->fd = open(devicefile, O_RDWR);
		if (dev->fd == -1) {
			printf("open failed: %s\n", devicefile);
			exit(1);
		}

		AUDIO_INITINFO(&ai);
		ai.mode = AUMODE_PLAY;
		ai.play.sample_rate = sc->sc_phwfmt.sample_rate;
		ai.play.encoding    = sc->sc_phwfmt.encoding;
		ai.play.precision   = sc->sc_phwfmt.precision;
		ai.play.channels    = sc->sc_phwfmt.channels;
		r = ioctl(dev->fd, AUDIO_SETINFO, &ai);
		if (r == -1) {
			printf("AUDIO_SETINFO failed\n");
			exit(1);
		}
	}
}

void
audio_detach(struct audio_softc *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;

	if (dev->fd != -1) {
		close(dev->fd);
		dev->fd = -1;
	}
	free(dev);
	sc->phys = NULL;
}

int
netbsd_start_output(void *hdl, void *blk, int blksize, void(*intr)(void *), void *arg)
{
	struct audio_softc *sc = hdl;
	audio_dev_netbsd_t *dev = sc->phys;

	lock(sc);

	int r = write(dev->fd, blk, blksize);
	if (r == -1) {
		printf("write failed: %s\n", strerror(errno));
		exit(1);
	}
	dev->sent_count += blksize / dev->frame_bytes;

	// 割り込み予約
	struct intr_t x;
	x.code = INTR_TRACKMIXER;
	x.sc = sc;
	x.func = intr;
	x.arg = arg;
	emu_intr(x);

	unlock(sc);
	return 0;
}

int
audio_softc_get_hw_capacity(struct audio_softc *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;
	// 2ブロック分
	return dev->frame_bytes * sc->sc_phwfmt.sample_rate * 40 / 1000 * 2;
}
