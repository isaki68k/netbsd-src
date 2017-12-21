#include "aumix.h"
#include "auintr.h"
#include "auring.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/audioio.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <pthread.h>

struct audio_dev_netbsd
{
	int fd;
	int frame_bytes;
	audio_format2_t fmt;
	int sent_count;
	pthread_mutex_t mutex;
	struct timeval tv;
};
typedef struct audio_dev_netbsd audio_dev_netbsd_t;

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
netbsd_start_output(void *hdl, void *blk, int blksize, void(*intr)(void *), void *arg)
{
	struct audio_softc *sc = hdl;
	audio_softc_play_start(sc);
	return 0;
}

int
netbsd_halt_output(void *hdl)
{
	return 0;
}

void
audio_attach(struct audio_softc **softc)
{
	struct audio_softc *sc;
	audio_dev_netbsd_t *dev;
	struct audio_info ai;
	int r;

	sc = calloc(1, sizeof(*sc));
	*softc = sc;
	sc->phys = calloc(1, sizeof(*dev));

	dev = sc->phys;
	dev->fd = open("/dev/sound", O_RDWR);

	dev->fmt.encoding = AUDIO_ENCODING_SLINEAR_LE;
	dev->fmt.channels = 2;
	dev->fmt.sample_rate = 48000;
	dev->fmt.precision = 16;
	dev->fmt.stride = 16;
	dev->frame_bytes = dev->fmt.precision / 8 * dev->fmt.channels;

	AUDIO_INITINFO(&ai);
	ai.mode = AUMODE_PLAY;
	ai.play.sample_rate = dev->fmt.sample_rate;
	ai.play.encoding    = dev->fmt.encoding;
	ai.play.precision   = dev->fmt.precision;
	ai.play.channels    = dev->fmt.channels;
	r = ioctl(dev->fd, AUDIO_SETINFO, &ai);
	if (r == -1) {
		printf("AUDIO_SETINFO failed\n");
		exit(1);
	}

	pthread_mutex_init(&dev->mutex, NULL);

	audio_softc_init(sc);
	audio_mixer_init(sc, sc->sc_pmixer, AUMODE_PLAY);
	audio_mixer_init(sc, sc->sc_rmixer, AUMODE_RECORD);

	sc->hw_if->allocm = netbsd_allocm;
	sc->hw_if->freem = netbsd_freem;
	sc->hw_if->start_output = netbsd_start_output;
	sc->hw_if->halt_output = netbsd_halt_output;
	sc->hw_hdl = sc;
}

void
audio_detach(struct audio_softc *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;

	close(dev->fd);
	free(dev);
	sc->phys = NULL;
}

void
audio_softc_play_start(struct audio_softc *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;
	audio_trackmixer_t *mixer = sc->sc_pmixer;

	if (mixer->hwbuf.count <= 0) return;
	if (dev->sent_count > 0) return;
printf("%s\n", __func__);

	lock(sc);

	int count;
	while ((count = audio_ring_unround_count(&mixer->hwbuf)) > 0) {
		int16_t *src = RING_TOP(int16_t, &mixer->hwbuf);
		int r = write(dev->fd, src, count * dev->frame_bytes);
		if (r == -1) {
			printf("write failed: %s\n", strerror(errno));
			exit(1);
		}
		audio_ring_tookfromtop(&mixer->hwbuf, count);
		dev->sent_count += count;

		// 転送終了時刻
		gettimeofday(&dev->tv, NULL);
		struct timeval d;
		d.tv_sec = 0;
		// 後ろの800は usec->msec に直す1000倍に、
		// ちょっと前倒しで 0.8 掛けたもの。
		d.tv_usec = r / (dev->fmt.sample_rate * dev->fmt.precision / 8 *
			dev->fmt.channels / 1000) * 800;
printf("usec=%d\n", (int)d.tv_usec);
		timeradd(&dev->tv, &d, &dev->tv);
	}

	unlock(sc);
}

bool
audio_softc_play_busy(struct audio_softc *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;

	lock(sc);
	if (dev->sent_count > 0) {
		struct timeval now, res;
		gettimeofday(&now, NULL);
		timersub(&dev->tv, &now, &res);
		if (res.tv_sec > 0) {
			// まだ転送完了時刻になってないのでビジーということにする
			unlock(sc);
			return true;
		}

#ifdef AUDIO_INTR_EMULATED
		struct intr_t x;
		x.code = INTR_TRACKMIXER;
		x.mixer = sc->sc_pmixer;
		x.count = dev->sent_count;
		emu_intr(x);
#else
		audio_trackmixer_intr(&sc->sc_pmixer, dev->sent_count);
#endif
		dev->sent_count = 0;
	}
	unlock(sc);
	return false;
}

int
audio_softc_get_hw_capacity(struct audio_softc *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;
	// 2ブロック分
	return dev->frame_bytes * dev->fmt.sample_rate * 40 / 1000 * 2;
}

void *
audio_softc_allocm(struct audio_softc *sc, int n)
{
	return malloc(n);
}

audio_format2_t
audio_softc_get_hw_format(struct audio_softc *sc, int mode)
{
	audio_dev_netbsd_t *dev = sc->phys;
	return dev->fmt;
}

void
WAIT()
{
	usleep(1);
}
