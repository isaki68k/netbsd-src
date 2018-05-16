// vi:set ts=8:
#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/kmem.h>

#include <sys/audioio.h>
#include <dev/audio_if.h>

#include <machine/bus.h>
#include <machine/cpu.h>

#include <arch/x68k/dev/dmacvar.h>
#include <arch/x68k/dev/intiovar.h>

#define MERCURY_MONO	1

#define MERCURY_ADDR	(0xecc080)
#define MERCURY_SIZE	(0x80)

#define MERC_DATA	(0xecc080 - MERCURY_ADDR)
#define MERC_CMD	(0xecc091 - MERCURY_ADDR)
#define MERC_STAT	(0xecc0a1 - MERCURY_ADDR)

#define MERC_CMD_HALF		(0x80)	/* Half rate */
#define MERC_CMD_INPSEL		(0x40)	/* Input select */
#define MERC_CMD_CLK_MASK	(0x30)	/* Clock bits */
#define MERC_CMD_CLK_EXT	(0x00)
#define MERC_CMD_CLK_32000	(0x10)
#define MERC_CMD_CLK_44100	(0x20)
#define MERC_CMD_CLK_48000	(0x30)
#define MERC_CMD_PAN_R		(0x08)	/* Panpot R */
#define MERC_CMD_PAN_L		(0x04)	/* Panpot L */
#define MERC_CMD_STEREO		(0x02)	/* Stereo(1)/Mono(0) */
#define MERC_CMD_OUT		(0x01)	/* Out(1)/In(0) */

#define MERC_STAT_INPSEL	MERC_CMD_INPSEL
#define MERC_STAT_CLK_MASK	MERC_CMD_CLK_MASK
#define MERC_STAT_32000		(0x08)
#define MERC_STAT_44100		(0x04)
#define MERC_STAT_48000		(0x02)
#define MERC_STAT_PLLERR	(0x01)

/* XXX copy from vs.c */
struct vs_dma {
	bus_dma_tag_t		vd_dmat;
	bus_dmamap_t		vd_map;
	void			*vd_addr;
	bus_dma_segment_t	vd_segs[1];
	int			vd_nsegs;
	size_t			vd_size;
	struct vs_dma		*vd_next;
};
#define KVADDR(dma)	((void *)(dma)->vd_addr)
#define KVADDR_END(dma) ((void *)((size_t)KVADDR(dma) + (dma)->vd_size)))
#define DMAADDR(dma)	((dma)->vd_map->dm_segs[0].ds_addr)

struct mercury_softc {
	device_t sc_dev;
	bus_space_handle_t sc_ioh;
	bus_space_tag_t sc_iot;
	uint8_t *sc_addr;

	uint8_t sc_cmd;
	void (*sc_intr)(void *);
	void *sc_arg;
	int sc_active;

	bus_dma_tag_t sc_dmat;
	struct dmac_channel_stat *sc_dma_ch;
	struct dmac_dma_xfer *sc_xfer;
	struct vs_dma *sc_dmas;
	struct vs_dma *sc_last_vd;

	kmutex_t sc_intr_lock;
	kmutex_t sc_thread_lock;
};

static int  mercury_match(device_t, cfdata_t, void *);
static void mercury_attach(device_t, device_t, void *);

static int  mercury_dmaintr(void *);
static int  mercury_dmaerrintr(void *);
static int  mercury_dmamem_alloc(struct mercury_softc *, size_t, size_t,
	size_t, struct vs_dma *);
static void mercury_dmamem_free(struct vs_dma *);

/* MI audio layer interface */
static int  mercury_open(void *, int);
static void mercury_close(void *);
#if defined(AUDIO2)
static int  mercury_query_format(void *, audio_format_query_t *);
static int  mercury_set_format(void *, int,
	const audio_params_t *, const audio_params_t *,
	audio_filter_reg_t *, audio_filter_reg_t *);
#else
static int  mercury_query_encoding(void *, struct audio_encoding *);
static int  mercury_set_params(void *, int, int, audio_params_t *,
	audio_params_t *, stream_filter_list_t *, stream_filter_list_t *);
#endif
static int  mercury_start_output(void *, void *, int, void (*)(void *), void *);
static int  mercury_start_input(void *, void *, int, void (*)(void *), void *);
static int  mercury_halt_output(void *);
static int  mercury_halt_input(void *);
static int  mercury_getdev(void *, struct audio_device *);
static int  mercury_set_port(void *, mixer_ctrl_t *);
static int  mercury_get_port(void *, mixer_ctrl_t *);
static int  mercury_query_devinfo(void *, mixer_devinfo_t *);
static void *mercury_allocm(void *, int, size_t);
static void mercury_freem(void *, void *addr, size_t);
static size_t mercury_round_buffersize(void *, int, size_t);
static int  mercury_get_props(void *);
static void mercury_get_locks(void *, kmutex_t **, kmutex_t **);

CFATTACH_DECL_NEW(mercury, sizeof(struct mercury_softc),
	mercury_match, mercury_attach, NULL, NULL);

static int mercury_attached;

static const struct audio_hw_if mercury_hw_if = {
	.open			= mercury_open,
	.close			= mercury_close,
#if defined(AUDIO2)
	.query_format		= mercury_query_format,
	.set_format		= mercury_set_format,
#else
	.query_encoding		= mercury_query_encoding,
	.set_params		= mercury_set_params,
#endif
	.start_output		= mercury_start_output,
	.start_input		= mercury_start_input,
	.halt_output		= mercury_halt_output,
	.halt_input		= mercury_halt_input,
	.getdev			= mercury_getdev,
	.set_port		= mercury_set_port,
	.get_port		= mercury_get_port,
	.query_devinfo		= mercury_query_devinfo,
	.allocm			= mercury_allocm,
	.freem			= mercury_freem,
	.round_buffersize	= mercury_round_buffersize,
	.get_props		= mercury_get_props,
	.get_locks		= mercury_get_locks,
};

static struct audio_device mercury_device = {
	"Mercury Unit",
	"",
	"mercury",
};

static const struct audio_format mercury_formats[] = {
	{ NULL, AUMODE_PLAY | AUMODE_RECORD, AUDIO_ENCODING_SLINEAR_BE, 16, 16,
#if defined(MERCURY_MONO)
	  1, AUFMT_MONAURAL,
#else
	  2, AUFMT_STEREO,
#endif
//	  6, { 16000, 22050, 24000, 32000, 44100, 48000 } },
	  1, { 16000 } },
};


static int
mercury_match(device_t parent, cfdata_t cf, void *aux)
{
	struct intio_attach_args *ia = aux;

	if (mercury_attached)
		return 0;

	if (ia->ia_addr == INTIOCF_ADDR_DEFAULT)
		ia->ia_addr = MERCURY_ADDR;

	/* Check whether the board is exists or not */
	if (badaddr((void *)IIOV(ia->ia_addr)))
		return 0;

	return 1;
}

static void
mercury_attach(device_t parent, device_t self, void *aux)
{
	struct mercury_softc *sc;
	struct intio_attach_args *ia;
	bus_space_handle_t ioh;
	int r;

	sc = device_private(self);
	sc->sc_dev = self;
	ia = aux;

	printf("\n");

	/* Re-map I/O space */
	r = bus_space_map(ia->ia_bst, ia->ia_addr, MERCURY_SIZE, 0, &ioh);
	if (r != 0) {
		aprint_normal_dev(sc->sc_dev, "bus_space_map failed\n");
		return;
	}

	sc->sc_iot = ia->ia_bst;
	sc->sc_ioh = ioh;
	sc->sc_addr = (void *)ia->ia_addr;
	sc->sc_dmas = NULL;
	sc->sc_last_vd = NULL;
	mutex_init(&sc->sc_thread_lock, MUTEX_DEFAULT, IPL_NONE);
	mutex_init(&sc->sc_intr_lock, MUTEX_DEFAULT, IPL_SCHED);

	/* Initialize DMAC */
	sc->sc_dmat = ia->ia_dmat;
	sc->sc_dma_ch = dmac_alloc_channel(parent, ia->ia_dma, "mercury",
	    ia->ia_dmaintr,     mercury_dmaintr, sc,
	    ia->ia_dmaintr + 1, mercury_dmaerrintr, sc,
	    (DMAC_DCR_XRM_CSWOH | DMAC_DCR_OTYP_EASYNC | DMAC_DCR_OPS_16BIT),
	    (DMAC_OCR_SIZE_WORD | DMAC_OCR_REQG_EXTERNAL));

	mercury_attached = 1;
	aprint_normal_dev(sc->sc_dev, "Mercury Unit V2/V3\n");

	audio_attach_mi(&mercury_hw_if, sc, sc->sc_dev);
}

static int
mercury_dmaintr(void *hdl)
{
	struct mercury_softc *sc;

	sc = hdl;

	KASSERT(sc->sc_intr);

	mutex_spin_enter(&sc->sc_intr_lock);
	sc->sc_intr(sc->sc_arg);
	mutex_spin_exit(&sc->sc_intr_lock);

	return 0;
}

static int
mercury_dmaerrintr(void *hdl)
{
	struct mercury_softc *sc;

	sc = hdl;
	printf("%s: DMA transfer error\n", device_xname(sc->sc_dev));

	return 0;
}

static int
mercury_open(void *hdl, int flags)
{
	struct mercury_softc *sc;

	printf("%s: flags=0x%x\n", __func__, flags);
	sc = hdl;
	sc->sc_active = 0;

	return 0;
}

static void
mercury_close(void *hdl)
{
	printf("%s\n", __func__);
}

#if defined(AUDIO2)
static int
mercury_query_format(void *hdl, audio_format_query_t *afp)
{

	return audio_query_format(mercury_formats,
	    __arraycount(mercury_formats), afp);
}
#else
static int
mercury_query_encoding(void *hdl, struct audio_encoding *ae)
{

	printf("%s\n", __func__);

	switch (ae->index) {
	case 0:
		strcpy(ae->name, AudioEslinear_le);
		ae->encoding = AUDIO_ENCODING_SLINEAR_BE;
		ae->precision = 16;
		ae->flags = 0;
		return 0;
	}

	return EINVAL;
}
#endif

#if defined(AUDIO2)
static int
mercury_set_format(void *hdl, int setmode,
	const audio_params_t *play, const audio_params_t *rec,
	audio_filter_reg_t *pfil, audio_filter_reg_t *rfil)
{
	struct mercury_softc *sc;
	uint8_t cmd;

	sc = hdl;
	printf("%s: mode=%d %s/%dbit/%dch/%dHz\n", __func__,
	    setmode, audio_encoding_name(play->encoding),
	    play->precision, play->channels, play->sample_rate);

	/* *play and *rec are identical because !AUDIO_PROP_INDEPENDENT */

	if (play->encoding != AUDIO_ENCODING_SLINEAR_BE ||
	    play->precision != 16) {
		printf("%s: encoding not matched\n", __func__);
		return EINVAL;
	}

	cmd = 0;
	if (play->channels == 2)
		cmd |= MERC_CMD_STEREO;

	switch (play->sample_rate) {
	case 16000:
		cmd |= MERC_CMD_CLK_32000;
		break;
	case 22050:
		cmd |= MERC_CMD_CLK_44100;
		break;
	case 24000:
		cmd |= MERC_CMD_CLK_48000;
		break;
	case 32000:
		cmd |= MERC_CMD_CLK_32000 | MERC_CMD_HALF;
		break;
	case 44100:
		cmd |= MERC_CMD_CLK_44100 | MERC_CMD_HALF;
		break;
	case 48000:
		cmd |= MERC_CMD_CLK_48000 | MERC_CMD_HALF;
		break;
	default:
printf("%s: invalid sample_rate %d\n", __func__, play->sample_rate);
		return EINVAL;
	}

	sc->sc_cmd = cmd;
printf("%s: sc_cmd=%x\n", __func__, sc->sc_cmd);
	return 0;
}
#else
static int
mercury_set_params(void *hdl, int setmode, int usemode,
	audio_params_t *play, audio_params_t *rec,
	stream_filter_list_t *pfil, stream_filter_list_t *rfil)
{
	struct mercury_softc *sc;
	struct audio_params *p;
	int mode;
	uint8_t cmd;

	sc = hdl;
	printf("%s setmode=%x usemode=%x\n", __func__, setmode, usemode);
	cmd = 0;

	for (mode = AUMODE_RECORD; mode != -1;
	     mode = (mode == AUMODE_RECORD) ? AUMODE_PLAY : -1) {
		if ((setmode & mode) == 0)
			continue;

		if (mode == AUMODE_PLAY) {
			p = play;
			cmd |= MERC_CMD_OUT;
		} else {
			p = rec;
		}

printf("%s mode=%s %s %u/%ubit %uch %uHz\n", __func__,
(p == play) ? "play" : "rec", audio_encoding_name(p->encoding),
p->validbits, p->precision, p->channels, p->sample_rate);
		if (p->channels == 2) {
			cmd |= MERC_CMD_STEREO;
		}
		if (p->encoding == AUDIO_ENCODING_SLINEAR_BE
		 && p->precision == 16) {
			switch (p->sample_rate) {
			case 32000:
				cmd |= MERC_CMD_CLK_32000;
				break;
			case 44100:
				cmd |= MERC_CMD_CLK_44100;
				break;
			case 48000:
				cmd |= MERC_CMD_CLK_48000;
				break;
			default:
printf("%s EINVAL\n", __func__);
				return EINVAL;
			}
printf("%s ok %x\n", __func__, cmd);
		}
	}
	sc->sc_cmd = cmd;
	return 0;
}
#endif

static int
mercury_start_output(void *hdl, void *block, int blksize,
	void (*intr)(void *), void *intrarg)
{
	struct mercury_softc *sc;
	struct vs_dma *vd;
	struct dmac_channel_stat *chan;
	uint8_t cmd;

	sc = hdl;

	sc->sc_intr = intr;
	sc->sc_arg = intrarg;

	/* Find DMA buffer. */
	for (vd = sc->sc_dmas; vd != NULL; vd = vd->vd_next) {
		if (KVADDR(vd) <= block && block < KVADDR_END(vd)
			break;
	}
	if (vd == NULL) {
		printf("%s: start_output: bad addr %p\n",
		    device_xname(sc->sc_dev), block);
		return EINVAL;
	}

	chan = sc->sc_dma_ch;

	if (vd != sc->sc_last_vd) {
		sc->sc_xfer = dmac_prepare_xfer(chan, sc->sc_dmat,
		    vd->vd_map, DMAC_OCR_DIR_MTD,
		    (DMAC_SCR_MAC_COUNT_UP | DMAC_SCR_DAC_NO_COUNT),
		    sc->sc_addr + MERC_DATA);
		sc->sc_last_vd = vd;
	}
	dmac_start_xfer_offset(chan->ch_softc, sc->sc_xfer,
	    (int)block - (int)KVADDR(vd), blksize);

	if (sc->sc_active == 0) {
		cmd = sc->sc_cmd
		    | MERC_CMD_OUT | MERC_CMD_PAN_L | MERC_CMD_PAN_R;
printf("start cmd=0x%x\n", cmd);
		bus_space_write_1(sc->sc_iot, sc->sc_ioh, MERC_CMD, cmd);
		sc->sc_active = 1;
	}

	return 0;
}

static int
mercury_start_input(void *hdl, void *block, int blksize,
	void (*intr)(void *), void *intrarg)
{
	printf("%s\n", __func__);
	return 0;
}

static int
mercury_halt_output(void *hdl)
{
	struct mercury_softc *sc;

	printf("%s\n", __func__);
	sc = hdl;
	if (sc->sc_active) {
		bus_space_write_1(sc->sc_iot, sc->sc_ioh, MERC_CMD, sc->sc_cmd);
		dmac_abort_xfer(sc->sc_dma_ch->ch_softc, sc->sc_xfer);
		sc->sc_active = 0;
	}
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

static void *
mercury_allocm(void *hdl, int direction, size_t size)
{
	struct mercury_softc *sc;
	struct vs_dma *vd;
	int error;

	sc = hdl;
	vd = kmem_alloc(sizeof(*vd), KM_SLEEP);
	vd->vd_dmat = sc->sc_dmat;

	error = mercury_dmamem_alloc(sc, size, 32, 0, vd);
	if (error) {
		kmem_free(vd, sizeof(*vd));
		return NULL;
	}
	vd->vd_next = sc->sc_dmas;
	sc->sc_dmas = vd;

	return KVADDR(vd);
}

static void
mercury_freem(void *hdl, void *addr, size_t size)
{
	struct mercury_softc *sc;
	struct vs_dma *p, **pp;

	sc = hdl;
	for (pp = &sc->sc_dmas; (p = *pp) != NULL; pp = &p->vd_next) {
		if (KVADDR(p) == addr) {
			mercury_dmamem_free(p);
			*pp = p->vd_next;
			kmem_free(p, sizeof(*p));
			return;
		}
	}
}

static int
mercury_dmamem_alloc(struct mercury_softc *sc, size_t size, size_t align,
	size_t boundary, struct vs_dma *vd)
{
	int error;

#ifdef DIAGNOSTIC
	if (size > DMAC_MAXSEGSZ)
		panic("%s: maximum size exceeded, %d", __func__, (int)size);
#endif

	vd->vd_size = size;

	error = bus_dmamem_alloc(vd->vd_dmat, vd->vd_size, align, boundary,
	    vd->vd_segs,
	    sizeof(vd->vd_segs) / sizeof(vd->vd_segs[0]),
	    &vd->vd_nsegs, BUS_DMA_WAITOK);
	if (error)
		goto out;

	error = bus_dmamem_map(vd->vd_dmat, vd->vd_segs, vd->vd_nsegs,
	    vd->vd_size, &vd->vd_addr,
	    BUS_DMA_WAITOK | BUS_DMA_COHERENT);
	if (error)
		goto free;

	error = bus_dmamap_create(vd->vd_dmat, vd->vd_size, 1, DMAC_MAXSEGSZ,
	    0, BUS_DMA_WAITOK, &vd->vd_map);
	if (error)
		goto unmap;

	error = bus_dmamap_load(vd->vd_dmat, vd->vd_map, vd->vd_addr,
				vd->vd_size, NULL, BUS_DMA_WAITOK);
	if (error)
		goto destroy;

	return 0;

 destroy:
	bus_dmamap_destroy(vd->vd_dmat, vd->vd_map);
 unmap:
	bus_dmamem_unmap(vd->vd_dmat, vd->vd_addr, vd->vd_size);
 free:
	bus_dmamem_free(vd->vd_dmat, vd->vd_segs, vd->vd_nsegs);
 out:
	return error;
}

static void
mercury_dmamem_free(struct vs_dma *vd)
{

	bus_dmamap_unload(vd->vd_dmat, vd->vd_map);
	bus_dmamap_destroy(vd->vd_dmat, vd->vd_map);
	bus_dmamem_unmap(vd->vd_dmat, vd->vd_addr, vd->vd_size);
	bus_dmamem_free(vd->vd_dmat, vd->vd_segs, vd->vd_nsegs);
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

static void
mercury_get_locks(void *hdl, kmutex_t **intr, kmutex_t **thread)
{
	struct mercury_softc *sc = hdl;

	*intr = &sc->sc_intr_lock;
	*thread = &sc->sc_thread_lock;
}
