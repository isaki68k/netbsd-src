// vi:set ts=8:
#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/kmem.h>

#include <sys/audioio.h>
#include <dev/audio_if.h>

#include <machine/autoconf.h>

/* XXX copy from xp.c */
#define XP_SHM_BASE	0x71000000
#define XP_SHM_SIZE	0x00010000	/* 64KB for XP; rest 64KB for lance */

struct psgpam_softc {
	device_t sc_dev;
	vaddr_t sc_shm_base;
	vsize_t sc_shm_size;

	void (*sc_intr)(void *);
	void *sc_arg;

	kmutex_t sc_intr_lock;
	kmutex_t sc_thread_lock;
};

static int  psgpam_match(device_t, cfdata_t, void *);
static void psgpam_attach(device_t, device_t, void *);

/* MI audio layer interface */
static int  psgpam_open(void *, int);
static void psgpam_close(void *);
#if defined(AUDIO2)
static int  psgpam_query_format(void *, audio_format_query_t *);
static int  psgpam_set_format(void *, int,
	const audio_params_t *, const audio_params_t *,
	audio_filter_reg_t *, audio_filter_reg_t *);
#else
static int  psgpam_query_encoding(void *, struct audio_encoding *);
static int  psgpam_set_params(void *, int, int, audio_params_t *,
	audio_params_t *, stream_filter_list_t *, stream_filter_list_t *);
#endif
static int  psgpam_start_output(void *, void *, int, void (*)(void *), void *);
static int  psgpam_start_input(void *, void *, int, void (*)(void *), void *);
static int  psgpam_halt_output(void *);
static int  psgpam_halt_input(void *);
static int  psgpam_getdev(void *, struct audio_device *);
static int  psgpam_set_port(void *, mixer_ctrl_t *);
static int  psgpam_get_port(void *, mixer_ctrl_t *);
static int  psgpam_query_devinfo(void *, mixer_devinfo_t *);
static void *psgpam_allocm(void *, int, size_t);
static void psgpam_freem(void *, void *addr, size_t);
static size_t psgpam_round_buffersize(void *, int, size_t);
static int  psgpam_get_props(void *);
static void psgpam_get_locks(void *, kmutex_t **, kmutex_t **);

CFATTACH_DECL_NEW(psgpam, sizeof(struct psgpam_softc),
	psgpam_match, psgpam_attach, NULL, NULL);

static int psgpam_matched;

static const struct audio_hw_if psgpam_hw_if = {
	.open			= psgpam_open,
	.close			= psgpam_close,
#if defined(AUDIO2)
	.query_format		= psgpam_query_format,
	.set_format		= psgpam_set_format,
#else
	.query_encoding		= psgpam_query_encoding,
	.set_params		= psgpam_set_params,
#endif
	.start_output		= psgpam_start_output,
	.start_input		= psgpam_start_input,
	.halt_output		= psgpam_halt_output,
	.halt_input		= psgpam_halt_input,
	.getdev			= psgpam_getdev,
	.set_port		= psgpam_set_port,
	.get_port		= psgpam_get_port,
	.query_devinfo		= psgpam_query_devinfo,
	.allocm			= psgpam_allocm,
	.freem			= psgpam_freem,
	.round_buffersize	= psgpam_round_buffersize,
	.get_props		= psgpam_get_props,
	.get_locks		= psgpam_get_locks,
};

static struct audio_device psgpam_device = {
	"PSG PAM",
	"",
	"psgpam",
};

static const struct audio_format psgpam_formats[] = {
	{ NULL, AUMODE_PLAY | AUMODE_RECORD, AUDIO_ENCODING_SLINEAR_BE, 16, 16,
	  1, AUFMT_MONAURAL,
	  6, { 16000, 22050, 24000, 32000, 44100, 48000 } },
	{ NULL, AUMODE_PLAY | AUMODE_RECORD, AUDIO_ENCODING_SLINEAR_BE, 16, 16,
	  2, AUFMT_STEREO,
	  6, { 16000, 22050, 24000, 32000, 44100, 48000 } },
};

static int
psgpam_match(device_t parent, cfdata_t cf, void *aux)
{
	struct mainbus_attach_args *maa = aux;

	if (psgpam_matched)
		return 0;

	if (maa->ma_addr != XP_SHM_BASE)
		return 0;

	psgpam_matched = 1;
	return 1;
}

static void
psgpam_attach(device_t parent, device_t self, void *aux)
{
	struct psgpam_softc *sc;

	sc = device_private(self);
	sc->sc_dev = self;

	aprint_normal(": HD647180X I/O processor as PSG PAM\n");

	sc->sc_shm_base = XP_SHM_BASE;
	sc->sc_shm_size = XP_SHM_SIZE;

	mutex_init(&sc->sc_thread_lock, MUTEX_DEFAULT, IPL_NONE);
	mutex_init(&sc->sc_intr_lock, MUTEX_DEFAULT, IPL_SCHED);

	audio_attach_mi(&psgpam_hw_if, sc, sc->sc_dev);
}

static int
psgpam_open(void *hdl, int flags)
{
	//struct psgpam_softc *sc;

	printf("%s: flags=0x%x\n", __func__, flags);
	//sc = hdl;

	return 0;
}

static void
psgpam_close(void *hdl)
{
	printf("%s\n", __func__);
}

#if defined(AUDIO2)
static int
psgpam_query_format(void *hdl, audio_format_query_t *afp)
{

	return audio_query_format(psgpam_formats,
	    __arraycount(psgpam_formats), afp);
}
#else
static int
psgpam_query_encoding(void *hdl, struct audio_encoding *ae)
{

	printf("%s\n", __func__);

	if (ae->index < 0 || ae->index >= __arraycount(psgpam_formats))
		return EINVAL;

	ae->encoding = psgpam_formats[ae->index].encoding;
	strcpy(ae->name, audio_encoding_name(ae->encoding));
	ae->precision = psgpam_formats[ae->index].precision;
	ae->flags = 0;
	return 0;
}
#endif

#if defined(AUDIO2)
static int
psgpam_set_format(void *hdl, int setmode,
	const audio_params_t *play, const audio_params_t *rec,
	audio_filter_reg_t *pfil, audio_filter_reg_t *rfil)
{
	//struct psgpam_softc *sc;

	//sc = hdl;
	printf("%s: mode=%d %s/%dbit/%dch/%dHz\n", __func__,
	    setmode, audio_encoding_name(play->encoding),
	    play->precision, play->channels, play->sample_rate);

	/* *play and *rec are identical because !AUDIO_PROP_INDEPENDENT */

	return 0;
}
#else
static int
psgpam_set_params(void *hdl, int setmode, int usemode,
	audio_params_t *play, audio_params_t *rec,
	stream_filter_list_t *pfil, stream_filter_list_t *rfil)
{

	printf("%s setmode=%x usemode=%x\n", __func__, setmode, usemode);
	return 0;
}
#endif

static int
psgpam_start_output(void *hdl, void *block, int blksize,
	void (*intr)(void *), void *intrarg)
{
	struct psgpam_softc *sc;

	sc = hdl;

printf("%s block=%p blksize=%d\n", __func__, block, blksize);

	sc->sc_intr = intr;
	sc->sc_arg = intrarg;

	return 0;
}

static int
psgpam_start_input(void *hdl, void *block, int blksize,
	void (*intr)(void *), void *intrarg)
{
	printf("%s block=%p blksize=%d\n", __func__, block, blksize);
	return 0;
}

static int
psgpam_halt_output(void *hdl)
{
	//struct psgpam_softc *sc;

	printf("%s\n", __func__);
	return 0;
}

static int
psgpam_halt_input(void *hdl)
{
	printf("%s\n", __func__);
	return 0;
}

static int
psgpam_getdev(void *hdl, struct audio_device *ret)
{
	*ret = psgpam_device;
	return 0;
}

static int
psgpam_set_port(void *hdl, mixer_ctrl_t *mc)
{
	printf("%s\n", __func__);
	return 0;
}

static int
psgpam_get_port(void *hdl, mixer_ctrl_t *mc)
{
	printf("%s\n", __func__);
	return 0;
}

static int
psgpam_query_devinfo(void *hdl, mixer_devinfo_t *di)
{

	printf("%s %d\n", __func__, di->index);
	switch (di->index) {
	default:
		return EINVAL;
	}
	return 0;
}

static void *
psgpam_allocm(void *hdl, int direction, size_t size)
{
	printf("%s size=%d\n", __func__, size);
	return NULL;
}

static void
psgpam_freem(void *hdl, void *addr, size_t size)
{
	printf("%s size=%d\n", __func__, size);
}

static size_t
psgpam_round_buffersize(void *hdl, int direction, size_t bufsize)
{

	printf("%s\n", __func__);

	return bufsize;
}

static int
psgpam_get_props(void *hdl)
{
	return AUDIO_PROP_PLAYBACK | AUDIO_PROP_CAPTURE;
}

static void
psgpam_get_locks(void *hdl, kmutex_t **intr, kmutex_t **thread)
{
	struct psgpam_softc *sc = hdl;

	*intr = &sc->sc_intr_lock;
	*thread = &sc->sc_thread_lock;
}
