#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/audioio.h>

#include <dev/audio_if.h>
#include <dev/mulaw.h>

#include <machine/bus.h>
#include <machine/cpu.h>

#include <arch/x68k/dev/dmacvar.h>
#include <arch/x68k/dev/intiovar.h>

#define MERCURY_ADDR	(0xecc080)
#define MERCURY_SIZE	(0x80)

#define MERC_DATA		(0xecc080 - MERCURY_ADDR)
#define MERC_CMD		(0xecc091 - MERCURY_ADDR)
#define MERC_STAT		(0xecc0a1 - MERCURY_ADDR)

#define MERC_CMD_HALF			(0x80)	/* Half rate */
#define MERC_CMD_INPSEL			(0x40)	/* Input select */
#define MERC_CMD_CLK_MASK		(0x30)	/* Clock bits */
#define MERC_CMD_CLK_EXT		(0x00)
#define MERC_CMD_CLK_32000		(0x10)
#define MERC_CMD_CLK_44100		(0x20)
#define MERC_CMD_CLK_48000		(0x30)
#define MERC_CMD_PAN_R			(0x08)	/* Panpot R */
#define MERC_CMD_PAN_L			(0x04)	/* Panpot L */
#define MERC_CMD_STEREO			(0x02)	/* Stereo(1)/Mono(0) */
#define MERC_CMD_OUT			(0x01)	/* Out(1)/In(0) */

#define MERC_STAT_INPSEL		MERC_CMD_INPSEL
#define MERC_STAT_CLK_MASK		MERC_CMD_CLK_MASK
#define MERC_STAT_32000			(0x08)
#define MERC_STAT_44100			(0x04)
#define MERC_STAT_48000			(0x02)
#define MERC_STAT_PLLERR		(0x01)

static int  mercury_match(device_t, cfdata_t, void *);
static void mercury_attach(device_t, device_t, void *);

/* MI audio layer interface */
static int  mercury_open(void *, int);
static void mercury_close(void *);
static int  mercury_query_encoding(void *, struct audio_encoding *);
static int  mercury_set_params(void *, int, int, audio_params_t *,
	audio_params_t *, stream_filter_list_t *, stream_filter_list_t *);
static int  mercury_halt_output(void *);
static int  mercury_halt_input(void *);
static int  mercury_getdev(void *, struct audio_device *);
static int  mercury_set_port(void *, mixer_ctrl_t *);
static int  mercury_get_port(void *, mixer_ctrl_t *);
static int  mercury_query_devinfo(void *, mixer_devinfo_t *);
static size_t mercury_round_buffersize(void *, int, size_t);
static int  mercury_get_props(void *);
static int  mercury_trigger_output(void *, void *, void *, int,
	void (*)(void *), void *, const audio_params_t *);
static int  mercury_trigger_input(void *, void *, void *, int,
	void (*)(void *), void *, const audio_params_t *);
static void mercury_get_locks(void *, kmutex_t **, kmutex_t **);

struct mercury_softc {
	device_t sc_dev;
	bus_space_handle_t sc_ioh;
	bus_space_tag_t sc_iot;

	kmutex_t sc_intr_lock;
	kmutex_t sc_thread_lock;
};

CFATTACH_DECL_NEW(mercury, sizeof(struct mercury_softc),
	mercury_match,
	mercury_attach,
 NULL, NULL);

static int mercury_attached;

static const struct audio_hw_if mercury_hw_if = {
	.open = mercury_open,
	.close = mercury_close,
	/* drain */
	.query_encoding = mercury_query_encoding,
	.set_params = mercury_set_params,
	/* round_blocksize */
	/* commit_settings */
	/* init_output */
	/* init_input */
	/* start_output */
	/* start_input */
	.halt_output = mercury_halt_output,
	.halt_input = mercury_halt_input,
	/* speaker_ctl */
	.getdev = mercury_getdev,
	/* setfd */
	.set_port = mercury_set_port,
	.get_port = mercury_get_port,
	.query_devinfo = mercury_query_devinfo,
	//.allocm = mercury_allocm,
	//.freem = mercury_freem,
	.round_buffersize = mercury_round_buffersize,
	/* mappage */
	.get_props = mercury_get_props,
	.trigger_output = mercury_trigger_output,
	.trigger_input = mercury_trigger_input,
	/* dev_ioctl */
	.get_locks = mercury_get_locks,
};

static struct audio_device mercury_device = {
	"Mercury Unit",
	"",
	"mercury",
};


static int
mercury_match(device_t parent, cfdata_t cf, void *aux)
{
	struct intio_attach_args *ia = aux;

	if (mercury_attached)
		return 0;

	if (ia->ia_addr == INTIOCF_ADDR_DEFAULT)
		ia->ia_addr = MERCURY_ADDR;

	/* fixed parameters */
	if (ia->ia_addr != MERCURY_ADDR)
		return 0;

	/* Check whether the board is exists or not */
	if (badaddr((void *)IIOV(ia->ia_addr)))
		return 0;

	return 1;
}

static void
mercury_attach(device_t parent, device_t self, void *aux)
{
	struct mercury_softc *sc = device_private(self);
	struct intio_attach_args *ia = aux;
	bus_space_handle_t ioh;

	printf("\n");
	sc->sc_dev = self;

	/* Re-map I/O space */
	if (bus_space_map(ia->ia_bst, ia->ia_addr, MERCURY_SIZE, 0, &ioh) != 0) {
		aprint_normal_dev(sc->sc_dev, "bus_space_map failed\n");
		return;
	}

	sc->sc_iot = ia->ia_bst;
	sc->sc_ioh = ioh;
	mutex_init(&sc->sc_thread_lock, MUTEX_DEFAULT, IPL_NONE);
	mutex_init(&sc->sc_intr_lock, MUTEX_DEFAULT, IPL_SCHED);

	mercury_attached = 1;
	aprint_normal_dev(sc->sc_dev, "Mercury Unit V2/V3\n");

	audio_attach_mi(&mercury_hw_if, sc, sc->sc_dev);
}

static int
mercury_open(void *hdl, int flags)
{
	printf("%s: flags=0x%x\n", __func__, flags);
	return 0;
}

static void
mercury_close(void *hdl)
{
	printf("%s\n", __func__);
}

static int
mercury_query_encoding(void *hdl, struct audio_encoding *ae)
{

	printf("%s\n", __func__);

	switch (ae->index) {
	case 0:
		strcpy(ae->name, AudioEslinear_le);
		ae->encoding = AUDIO_ENCODING_SLINEAR_LE;
		ae->precision = 16;
		ae->flags = 0;
		return 0;
	}

	return EINVAL;
}

static int
mercury_set_params(void *hdl, int setmode, int usemode,
	audio_params_t *play, audio_params_t *rec,
	stream_filter_list_t *pfil, stream_filter_list_t *rfil)
{
	struct audio_params *p;
	int mode;

	printf("%s setmode=%x usemode=%x\n", __func__, setmode, usemode);

	for (mode = AUMODE_RECORD; mode != -1;
	     mode = (mode == AUMODE_RECORD) ? AUMODE_PLAY : -1) {
		if ((setmode & mode) == 0)
			continue;

		p = (mode == AUMODE_PLAY) ? play : rec;

printf("%s enc=%u rate=%u prec=%u/%u ch=%u\n", __func__,
p->encoding, p->sample_rate, p->validbits, p->precision, p->channels);
		if (p->encoding == AUDIO_ENCODING_SLINEAR_BE
		 && p->precision == 16) {
			switch (p->sample_rate) {
			case 32000:
			case 44100:
			case 48000:
printf("%s ok\n", __func__);
				return 0;
			default:
printf("%s EINVAL\n", __func__);
				return EINVAL;
			}
		}
	}
	return 0;
}

static int
mercury_halt_output(void *hdl)
{
	printf("%s\n", __func__);
	return 0;
}

static int
mercury_halt_input(void *hdl)
{
	printf("%s\n", __func__);
	return 0;
}

static int
mercury_getdev(void *hdl, struct audio_device *ret)
{
	*ret = mercury_device;
	return 0;
}

static int
mercury_set_port(void *hdl, mixer_ctrl_t *mc)
{
	printf("%s\n", __func__);
	return 0;
}

static int
mercury_get_port(void *hdl, mixer_ctrl_t *mc)
{
	printf("%s\n", __func__);
	return 0;
}

static int
mercury_query_devinfo(void *hdl, mixer_devinfo_t *di)
{

	printf("%s %d\n", __func__, di->index);
	switch (di->index) {
	default:
		return EINVAL;
	}
	return 0;
}

static size_t
mercury_round_buffersize(void *hdl, int direction, size_t bufsize)
{

	printf("%s\n", __func__);

	if (bufsize > DMAC_MAXSEGSZ)
		bufsize = DMAC_MAXSEGSZ;
	return bufsize;
}

static int
mercury_get_props(void *hdl)
{
	return AUDIO_PROP_PLAYBACK | AUDIO_PROP_CAPTURE;
}

static int
mercury_trigger_output(void *hdl, void *start, void *end, int blksize,
	void (*intr)(void *), void *intrarg, const audio_params_t *param)
{
	printf("%s\n", __func__);
	return 0;
}

static int
mercury_trigger_input(void *hdl, void *start, void *end, int blksize,
	void (*intr)(void *), void *intrarg, const audio_params_t *param)
{
	printf("%s\n", __func__);
	return 0;
}

static void
mercury_get_locks(void *hdl, kmutex_t **intr, kmutex_t **thread)
{
	struct mercury_softc *sc = hdl;

	printf("%s\n", __func__);
	*intr = &sc->sc_intr_lock;
	*thread = &sc->sc_thread_lock;
}
