#include "aumix.h"
#include "auring.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/audioio.h>
#include <sys/ioctl.h>

struct audio_dev_netbsd
{
	int fd;
	int frame_bytes;
	audio_format_t fmt;
};
typedef struct audio_dev_netbsd audio_dev_netbsd_t;

void
lock(audio_softc_t *sc)
{
}

void
unlock(audio_softc_t *sc)
{
}

void
audio_attach(audio_softc_t **softc)
{
	audio_softc_t *sc;
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
	dev->fmt.frequency = 48000;
	dev->fmt.precision = 16;
	dev->fmt.stride = 16;

	AUDIO_INITINFO(&ai);
	ai.mode = AUMODE_PLAY;
	ai.play.sample_rate = dev->fmt.frequency;
	ai.play.encoding    = dev->fmt.encoding;
	ai.play.precision   = dev->fmt.precision;
	ai.play.channels    = dev->fmt.channels;
	r = ioctl(dev->fd, AUDIO_SETINFO, &ai);
	if (r == -1) {
		printf("AUDIO_SETINFO failed\n");
		exit(1);
	}
	dev->frame_bytes = ai.play.precision / 8 * ai.play.channels;
printf("frame_bytes=%d\n", dev->frame_bytes);

	audio_mixer_init(&sc->mixer_play, sc, AUDIO_PLAY);
	audio_mixer_init(&sc->mixer_rec, sc, AUDIO_REC);
}

void
audio_detach(audio_softc_t *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;

	close(dev->fd);
	free(dev);
	sc->phys = NULL;
}

void
audio_softc_play_start(audio_softc_t *sc)
{
	audio_dev_netbsd_t *dev = sc->phys;
	audio_lanemixer_t *mixer = &sc->mixer_play;

	lock(sc);

	int count;
	int total = 0;
	for (int loop = 0; loop < 2; loop++) {
		count = audio_ring_unround_count(&mixer->hw_buf);

		int16_t *src = RING_TOP(int16_t, &mixer->hw_buf);
		
		int bytelen = count * dev->frame_bytes;
		int r = write(dev->fd, src, bytelen);
		if (r == -1) {
			printf("write failed: %s\n", strerror(errno));
			exit(1);
		}
		total += r;

		audio_ring_tookfromtop(&mixer->hw_buf, count);
	}

	// intr?
	audio_lanemixer_intr(mixer, total);

	unlock(sc);
}

bool
audio_softc_play_busy(audio_softc_t *sc)
{
	return 0;	/* XXX */
}

int
audio_softc_get_hw_capacity(audio_softc_t *sc)
{
	return 65536;	/* XXX */
}

void *
audio_softc_allocm(audio_softc_t *sc, int n)
{
	return malloc(n);
}

audio_format_t
audio_softc_get_hw_format(audio_softc_t *sc, int mode)
{
	audio_dev_netbsd_t *dev = sc->phys;
	return dev->fmt;
}

void
WAIT()
{
	usleep(1);
}
