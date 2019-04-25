/*
 *
 */
/*
 * EMU10K1 single voice driver
 * o. only 1 voice
 * o. only s16le 2ch 48k
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: emuxki.c,v 1.67 2019/03/16 12:09:58 isaki Exp $");

#include <sys/param.h>
#include <sys/device.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/audioio.h>
#include <sys/mutex.h>
#include <sys/kmem.h>
#include <sys/malloc.h>
#include <sys/fcntl.h>

#include <sys/bus.h>
#include <sys/intr.h>

#include <dev/audio_if.h>
#include <dev/audiovar.h>

#include <dev/ic/ac97reg.h>
#include <dev/ic/ac97var.h>

#include <dev/pci/pcidevs.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <dev/pci/emuxkireg.h>
#include <dev/pci/emuxkivar.h>

#define AUDIO_DEBUG 1
#ifdef AUDIO_DEBUG
extern int audiodebug;
#define emudebug audiodebug
# define DPRINTF(x)	if (emudebug) printf x
# define DPRINTFN(n,x)	if (emudebug>=(n)) printf x
#else
# define DPRINTF(x)
# define DPRINTFN(n,x)
#endif

/* blackmagic */
#define X1(x) (sc->sc_type & EMUXKI_AUDIGY) ? EMU_A_##x : EMU_##x
#define X2(x, y) (sc->sc_type & EMUXKI_AUDIGY) ? EMU_A_##x(EMU_A_##y) : EMU_##x(EMU_##y)

#define EMU_A_DSP_FX		EMU_DSP_FX
#define EMU_A_DSP_IN_AC97	EMU_DSP_IN_AC97



/* prototypes */
static struct dmamem * dmamem_alloc(struct emuxki_softc *sc, size_t size);
static void
dmamem_free(struct dmamem *mem);
static void
dmamem_sync(struct dmamem *mem, int ops);
static uint32_t
emuxki_readio_4(struct emuxki_softc *sc, int addr);
static void
emuxki_writeio_4(struct emuxki_softc *sc, int addr, uint32_t data);
static uint32_t
emuxki_readptr(struct emuxki_softc *sc, int aptr, int dptr,
    int addr);
static void
emuxki_writeptr(struct emuxki_softc *sc, int aptr, int dptr,
    int addr, uint32_t data);
static uint32_t
emuxki_read(struct emuxki_softc *sc, int ch, int addr);
static void
emuxki_write(struct emuxki_softc *sc, int ch, int addr, uint32_t data);
static int
emuxki_match(device_t parent, cfdata_t match, void *aux);
static void
emuxki_attach(device_t parent, device_t self, void *aux);
static int
emuxki_detach(device_t self, int flags);
static int
emuxki_init(struct emuxki_softc *sc);
static void
emuxki_dsp_addop(struct emuxki_softc *sc, uint16_t *pc, uint8_t op,
		  uint16_t r, uint16_t a, uint16_t x, uint16_t y);
static void
emuxki_initfx(struct emuxki_softc *sc);
static void
emuxki_play_start(struct emuxki_softc *sc, int ch,
    uint32_t start, uint32_t end);
static void
emuxki_play_stop(struct emuxki_softc *sc, int ch);

static int
emuxki_open(void *addr, int flags);
static void
emuxki_close(void *addr);
static int
emuxki_query_format(void *addr, audio_format_query_t *afp);
static int
emuxki_set_format(void *addr, int setmode,
	const audio_params_t *play, const audio_params_t *rec,
	audio_filter_reg_t *pfil, audio_filter_reg_t *rfil);
static int
emuxki_halt_output(void *addr);
static int
emuxki_halt_input(void *addr);
static int
emuxki_intr(void *arg);
static int
emuxki_getdev(void *addr, struct audio_device *dev);
static int
emuxki_set_port(void *addr, mixer_ctrl_t *mctl);
static int
emuxki_get_port(void *addr, mixer_ctrl_t *mctl);
static int
emuxki_query_devinfo(void *addr, mixer_devinfo_t *minfo);
static void *
emuxki_allocm(void *addr, int direction, size_t size);
static void
emuxki_freem(void *addr, void *ptr, size_t size);
static int
emuxki_round_blocksize(void *addr, int blksize,
    int mode, const audio_params_t* param);
static size_t
emuxki_round_buffersize(void *addr, int direction, size_t bsize);
static int
emuxki_get_props(void *addr);
static int
emuxki_trigger_output(void *addr, void *start, void *end, int blksize,
    void (*inth) (void *), void *inthparam, const audio_params_t *params);
static int
emuxki_trigger_input(void *addr, void *start, void *end, int blksize,
    void (*inth) (void *), void *inthparam, const audio_params_t *params);
static void
emuxki_get_locks(void *arg, kmutex_t **intr, kmutex_t **proc);

static int
emuxki_ac97_init(struct emuxki_softc *sc);
static int
emuxki_ac97_attach(void *arg, struct ac97_codec_if *codecif);
static int
emuxki_ac97_read(void *arg, uint8_t reg, uint16_t *val);
static int
emuxki_ac97_write(void *arg, uint8_t reg, uint16_t val);
static int
emuxki_ac97_reset(void *arg);
static enum ac97_host_flags
emuxki_ac97_flags(void *arg);

/* CF DECL */
CFATTACH_DECL_NEW(emuxki, sizeof(struct emuxki_softc),
    emuxki_match, emuxki_attach, emuxki_detach, NULL);

static const struct audio_hw_if emuxki_hw_if = {
	.open			= emuxki_open,
	.close			= emuxki_close,
	.query_format		= emuxki_query_format,
	.set_format		= emuxki_set_format,
	.round_blocksize	= emuxki_round_blocksize,
	.halt_output		= emuxki_halt_output,
	.halt_input		= emuxki_halt_input,
	.getdev			= emuxki_getdev,
	.set_port		= emuxki_set_port,
	.get_port		= emuxki_get_port,
	.query_devinfo		= emuxki_query_devinfo,
	.allocm			= emuxki_allocm,
	.freem			= emuxki_freem,
	.round_buffersize	= emuxki_round_buffersize,
	.get_props		= emuxki_get_props,
	.trigger_output		= emuxki_trigger_output,
	.trigger_input		= emuxki_trigger_input,
	.get_locks		= emuxki_get_locks,
};

#define EMUXKI_FORMAT_S16(aumode, ch, chmask) \
	{ \
		.mode		= (aumode), \
		.encoding	= AUDIO_ENCODING_SLINEAR_LE, \
		.validbits	= 16, \
		.precision	= 16, \
		.channels	= (ch), \
		.channel_mask	= (chmask), \
		.frequency_type	= 1, \
		.frequency	= { 48000 }, \
	}
static const struct audio_format emuxki_formats[] = {
	EMUXKI_FORMAT_S16(AUMODE_PLAY | AUMODE_RECORD, 2, AUFMT_STEREO),
};
#define EMUXKI_NFORMATS	__arraycount(emuxki_formats)

/* dma memory */

static struct dmamem *
dmamem_alloc(struct emuxki_softc *sc, size_t size)
{
	struct dmamem	*mem;

	KASSERT(!mutex_owned(&sc->sc_intr_lock));

	/* Allocate memory for structure */
	mem = kmem_alloc(sizeof(*mem), KM_SLEEP);
	mem->dmat = sc->sc_dmat;
	mem->size = size;
	mem->align = EMU_DMA_ALIGN;
	mem->nsegs = EMU_DMA_NSEGS;
	mem->bound = 0;

	mem->segs = kmem_alloc(mem->nsegs * sizeof(*(mem->segs)), KM_SLEEP);

	if (bus_dmamem_alloc(mem->dmat, mem->size, mem->align, mem->bound,
			     mem->segs, mem->nsegs, &(mem->rsegs),
			     BUS_DMA_WAITOK)) {
		DPRINTF(("%s bus_dmamem_alloc failed\n", __func__));
		goto memfree;
	}

	if (bus_dmamem_map(mem->dmat, mem->segs, mem->nsegs, mem->size,
			   &(mem->kaddr), BUS_DMA_WAITOK | BUS_DMA_COHERENT)) {
		DPRINTF(("%s bus_dmamem_map failed\n", __func__));
		goto free;
	}

	if (bus_dmamap_create(mem->dmat, mem->size, mem->nsegs, mem->size,
			      mem->bound, BUS_DMA_WAITOK, &(mem->map))) {
		DPRINTF(("%s bus_dmamap_create failed\n", __func__));
		goto unmap;
	}

	if (bus_dmamap_load(mem->dmat, mem->map, mem->kaddr,
			    mem->size, NULL, BUS_DMA_WAITOK)) {
		DPRINTF(("%s bus_dmamap_load failed\n", __func__));
		goto destroy;
	}

	return mem;

 destroy:
	bus_dmamap_destroy(mem->dmat, mem->map);
 unmap:
	bus_dmamem_unmap(mem->dmat, mem->kaddr, mem->size);
 free:
	bus_dmamem_free(mem->dmat, mem->segs, mem->nsegs);
 memfree:
	kmem_free(mem->segs, mem->nsegs * sizeof(*(mem->segs)));
	kmem_free(mem, sizeof(*mem));

	return NULL;
}

static void
dmamem_free(struct dmamem *mem)
{

	bus_dmamap_unload(mem->dmat, mem->map);
	bus_dmamap_destroy(mem->dmat, mem->map);
	bus_dmamem_unmap(mem->dmat, mem->kaddr, mem->size);
	bus_dmamem_free(mem->dmat, mem->segs, mem->nsegs);
	
	kmem_free(mem->segs, mem->nsegs * sizeof(*(mem->segs)));
	kmem_free(mem, sizeof(*mem));
}

static void
dmamem_sync(struct dmamem *mem, int ops)
{
	bus_dmamap_sync(mem->dmat, mem->map, 0, mem->size, ops);
}


/* I/O register access */
static uint8_t
emuxki_readio_1(struct emuxki_softc *sc, int addr) __unused;

static uint8_t
emuxki_readio_1(struct emuxki_softc *sc, int addr)
{
	return bus_space_read_1(sc->sc_iot, sc->sc_ioh, addr);
	//return bus_space_read_1(sc->memt, sc->memh, addr);
}

static void
emuxki_writeio_1(struct emuxki_softc *sc, int addr, uint8_t data)
{
	bus_space_write_1(sc->sc_iot, sc->sc_ioh, addr, data);
	//bus_space_write_1(sc->memt, sc->memh, addr, data);
}

static uint16_t
emuxki_readio_2(struct emuxki_softc *sc, int addr)
{
	return bus_space_read_2(sc->sc_iot, sc->sc_ioh, addr);
	//return bus_space_read_2(sc->memt, sc->memh, addr);
}

static void
emuxki_writeio_2(struct emuxki_softc *sc, int addr, uint16_t data)
{
	bus_space_write_2(sc->sc_iot, sc->sc_ioh, addr, data);
	//bus_space_write_2(sc->memt, sc->memh, addr, data);
}

static uint32_t
emuxki_readio_4(struct emuxki_softc *sc, int addr)
{
	return bus_space_read_4(sc->sc_iot, sc->sc_ioh, addr);
	//return bus_space_read_4(sc->memt, sc->memh, addr);
}

static void
emuxki_writeio_4(struct emuxki_softc *sc, int addr, uint32_t data)
{
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, addr, data);
	//bus_space_write_4(sc->memt, sc->memh, addr, data);
}

static uint32_t
emuxki_readptr(struct emuxki_softc *sc, int aptr, int dptr,
    int addr)
{
	uint32_t data;

	mutex_spin_enter(&sc->sc_index_lock);
	emuxki_writeio_4(sc, aptr, addr);
	data = emuxki_readio_4(sc, dptr);
	mutex_spin_exit(&sc->sc_index_lock);
	return data;
}

static void
emuxki_writeptr(struct emuxki_softc *sc, int aptr, int dptr,
    int addr, uint32_t data)
{
	mutex_spin_enter(&sc->sc_index_lock);
	emuxki_writeio_4(sc, aptr, addr);
	emuxki_writeio_4(sc, dptr, data);
	mutex_spin_exit(&sc->sc_index_lock);
}

static uint32_t
emuxki_read(struct emuxki_softc *sc, int ch, int addr)
{
	// original HENTAI addressing is never supported
	KASSERT((addr & 0xff000000) == 0);

	return emuxki_readptr(sc, EMU_PTR, EMU_DATA, (addr << 16) + ch);
}

static void
emuxki_write(struct emuxki_softc *sc, int ch, int addr, uint32_t data)
{
	// original HENTAI addressing is never supported
	KASSERT((addr & 0xff000000) == 0);

	emuxki_writeptr(sc, EMU_PTR, EMU_DATA, (addr << 16) + ch, data);
}

/*
 * MD driver
 */

static int
emuxki_match(device_t parent, cfdata_t match, void *aux)
{
	struct pci_attach_args *pa;

	pa = aux;
	if (PCI_VENDOR(pa->pa_id) != PCI_VENDOR_CREATIVELABS)
		return 0;

	switch (PCI_PRODUCT(pa->pa_id)) {
	case PCI_PRODUCT_CREATIVELABS_SBLIVE:
	case PCI_PRODUCT_CREATIVELABS_SBLIVE2:
	case PCI_PRODUCT_CREATIVELABS_AUDIGY:
		return 1;
	default:
		return 0;
	}
}


static void
emuxki_attach(device_t parent, device_t self, void *aux)
{
	struct emuxki_softc *sc;
	struct pci_attach_args *pa;
	pci_intr_handle_t ih;
	const char *intrstr;
	char intrbuf[PCI_INTRSTR_LEN];
	pcireg_t reg;

	sc = device_private(self);
	sc->sc_dev = self;
	pa = aux;

	pci_aprint_devinfo(pa, "Audio controller");
printf("dmat=%p\n", (char*)pa->pa_dmat);

	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_NONE);
	mutex_init(&sc->sc_intr_lock, MUTEX_DEFAULT, IPL_AUDIO);
	mutex_init(&sc->sc_index_lock, MUTEX_DEFAULT, IPL_AUDIO);

	sc->sc_pc   = pa->pa_pc;
	sc->sc_dmat = pa->pa_dmat;

	reg = pci_conf_read(pa->pa_pc, pa->pa_tag, PCI_COMMAND_STATUS_REG);
	reg |= PCI_COMMAND_IO_ENABLE | PCI_COMMAND_MASTER_ENABLE |
		PCI_COMMAND_MEM_ENABLE;
	pci_conf_write(pa->pa_pc, pa->pa_tag, PCI_COMMAND_STATUS_REG, reg);

	if (pci_mapreg_map(pa, EMU_PCI_CBIO, PCI_MAPREG_TYPE_IO, 0,
	    &(sc->sc_iot), &(sc->sc_ioh), &(sc->sc_iob),
			   &(sc->sc_ios))) {
		aprint_error(": can't map iospace\n");
		return;
	}

	if (pci_intr_map(pa, &ih)) {
		aprint_error_dev(self, "couldn't map interrupt\n");
		goto unmap;
	}

	intrstr = pci_intr_string(pa->pa_pc, ih, intrbuf, sizeof(intrbuf));
	sc->sc_ih = pci_intr_establish_xname(pa->pa_pc, ih, IPL_AUDIO,
	    emuxki_intr, sc, device_xname(self));
	if (sc->sc_ih == NULL) {
		aprint_error_dev(self, "couldn't establish interrupt");
		if (intrstr != NULL)
			aprint_error(" at %s", intrstr);
		aprint_error("\n");
		goto unmap;
	}
	aprint_normal_dev(self, "interrupting at %s\n", intrstr);

	/* XXX it's unknown whether APS is made from Audigy as well */
	if (PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_CREATIVELABS_AUDIGY) {
		sc->sc_type = EMUXKI_AUDIGY;
		if (PCI_REVISION(pa->pa_class) == 0x04) {
			sc->sc_type |= EMUXKI_AUDIGY2;
			strlcpy(sc->sc_audv.name, "Audigy2",
			    sizeof(sc->sc_audv.name));
		} else {
			strlcpy(sc->sc_audv.name, "Audigy",
			    sizeof(sc->sc_audv.name));
		}
	} else if (pci_conf_read(pa->pa_pc, pa->pa_tag,
	    PCI_SUBSYS_ID_REG) == EMU_SUBSYS_APS) {
		sc->sc_type = EMUXKI_APS;
		strlcpy(sc->sc_audv.name, "E-mu APS", sizeof(sc->sc_audv.name));
	} else {
		sc->sc_type = EMUXKI_SBLIVE;
		strlcpy(sc->sc_audv.name, "SB Live!", sizeof(sc->sc_audv.name));
	}
	snprintf(sc->sc_audv.version, sizeof(sc->sc_audv.version), "0x%02x",
		 PCI_REVISION(pa->pa_class));
	strlcpy(sc->sc_audv.config, "emuxki", sizeof(sc->sc_audv.config));

	if (emuxki_init(sc)) {
		aprint_error("emuxki_init error\n");
		goto intrdis;
	}
	if (emuxki_ac97_init(sc)) {
		aprint_error("emuxki_ac97_init error\n");
		goto intrdis;
	}

	sc->mode = 0;

	sc->pmem = NULL;
	sc->pintr = NULL;
	sc->pintrarg = NULL;
	memset(&sc->play, 0, sizeof(sc->play));
	sc->pframesize = 0;
	sc->pblksize = 0;
	sc->plength = 0;
	sc->poffset = 0;

	sc->rmem = NULL;
	sc->rintr = NULL;
	sc->rintrarg = NULL;
	memset(&sc->rec, 0, sizeof(sc->rec));

	sc->timer = 0;

	sc->sc_audev = audio_attach_mi(&emuxki_hw_if, sc, self);
	if (sc->sc_audev == NULL) {
		aprint_error("audio_attach_mi error\n");
		goto intrdis;
	}

	return;

 intrdis:
	pci_intr_disestablish(sc->sc_pc, sc->sc_ih);
 unmap:
	bus_space_unmap(sc->sc_iot, sc->sc_ioh, sc->sc_ios);
	//bus_space_unmap(sc->memt, sc->memh, sc->sc_ios);
	return;
}

static int
emuxki_detach(device_t self, int flags)
{
	struct emuxki_softc *sc;

	sc = device_private(self);
	if (sc->sc_audev != NULL) /* Test in case audio didn't attach */
		config_detach(sc->sc_audev, 0);

	/* All voices should be stopped now but add some code here if not */
	emuxki_writeio_4(sc, EMU_HCFG,
		EMU_HCFG_LOCKSOUNDCACHE |
		EMU_HCFG_LOCKTANKCACHE_MASK |
		EMU_HCFG_MUTEBUTTONENABLE);
	emuxki_writeio_4(sc, EMU_INTE, 0);

	/* Disable any Channels interrupts */
	emuxki_write(sc, 0, EMU_CLIEL, 0);
	emuxki_write(sc, 0, EMU_CLIEH, 0);
	emuxki_write(sc, 0, EMU_SOLEL, 0);
	emuxki_write(sc, 0, EMU_SOLEH, 0);

	/* stop DSP */
	emuxki_write(sc, 0, X1(DBG), X1(DBG_SINGLE_STEP));

	dmamem_free(sc->ptb);

	pci_intr_disestablish(sc->sc_pc, sc->sc_ih);
	bus_space_unmap(sc->sc_iot, sc->sc_ioh, sc->sc_ios);
	//bus_space_unmap(sc->memt, sc->memh, sc->sc_ios);

	mutex_destroy(&sc->sc_lock);
	mutex_destroy(&sc->sc_intr_lock);
	mutex_destroy(&sc->sc_index_lock);

	return 0;
}

static int
emuxki_init(struct emuxki_softc *sc)
{
	int i;
	uint32_t spcs;

	/* clear AUDIO bit */
	emuxki_writeio_4(sc, EMU_HCFG,
		EMU_HCFG_LOCKSOUNDCACHE |
		EMU_HCFG_LOCKTANKCACHE_MASK |
		EMU_HCFG_MUTEBUTTONENABLE);

	/* mask interrupt without PCIERR */
	emuxki_writeio_4(sc, EMU_INTE,
		EMU_INTE_SAMPLERATER | /* always on this bit */
		EMU_INTE_PCIERRENABLE);

	/* disable all channel interrupt */
	emuxki_write(sc, 0, EMU_CLIEL, 0);
	emuxki_write(sc, 0, EMU_CLIEH, 0);
	emuxki_write(sc, 0, EMU_SOLEL, 0);
	emuxki_write(sc, 0, EMU_SOLEH, 0);
	
	/* Set recording buffers sizes to zero */
	emuxki_write(sc, 0, EMU_MICBS, EMU_RECBS_BUFSIZE_NONE);
	emuxki_write(sc, 0, EMU_MICBA, 0);
	emuxki_write(sc, 0, EMU_FXBS, EMU_RECBS_BUFSIZE_NONE);
	emuxki_write(sc, 0, EMU_FXBA, 0);
	emuxki_write(sc, 0, EMU_ADCBS, EMU_RECBS_BUFSIZE_NONE);
	emuxki_write(sc, 0, EMU_ADCBA, 0);

	if(sc->sc_type & EMUXKI_AUDIGY) {
		emuxki_write(sc, 0, EMU_SPBYPASS, EMU_SPBYPASS_24_BITS);
		emuxki_write(sc, 0, EMU_AC97SLOT,
			EMU_AC97SLOT_CENTER | EMU_AC97SLOT_LFE);
	}

	/* Initialize all channels to stopped and no effects */
	for (i = 0; i < EMU_NUMCHAN; i++) {
		emuxki_write(sc, i, EMU_CHAN_DCYSUSV, 0x7f7f);
		emuxki_write(sc, i, EMU_CHAN_IP, EMU_CHAN_IP_UNITY);
		emuxki_write(sc, i, EMU_CHAN_VTFT, 0xffff);
		emuxki_write(sc, i, EMU_CHAN_CVCF, 0xffff);
		emuxki_write(sc, i, EMU_CHAN_PTRX, 0);
		emuxki_write(sc, i, EMU_CHAN_CPF, 0);
		emuxki_write(sc, i, EMU_CHAN_CCR, 0);
		emuxki_write(sc, i, EMU_CHAN_PSST, 0);
		emuxki_write(sc, i, EMU_CHAN_DSL, 0x10);	/* Why 16 ? */
		emuxki_write(sc, i, EMU_CHAN_CCCA, EMU_CHAN_CCCA_INTERPROM_1);
		emuxki_write(sc, i, EMU_CHAN_Z1, 0);
		emuxki_write(sc, i, EMU_CHAN_Z2, 0);
		emuxki_write(sc, i, EMU_CHAN_MAPA, 0xffffffff);
		emuxki_write(sc, i, EMU_CHAN_MAPB, 0xffffffff);
		emuxki_write(sc, i, EMU_CHAN_FXRT, 0x32100000);
		emuxki_write(sc, i, EMU_CHAN_ATKHLDM, 0);
		emuxki_write(sc, i, EMU_CHAN_DCYSUSM, 0);
		emuxki_write(sc, i, EMU_CHAN_IFATN, 0xffff);
		emuxki_write(sc, i, EMU_CHAN_PEFE, 0x007f);
		emuxki_write(sc, i, EMU_CHAN_FMMOD, 0);
		emuxki_write(sc, i, EMU_CHAN_TREMFRQ, 0);
		emuxki_write(sc, i, EMU_CHAN_FM2FRQ2, 0);
		emuxki_write(sc, i, EMU_CHAN_TEMPENV, 0);

		/* these are last so OFF prevents writing */
		emuxki_write(sc, i, EMU_CHAN_LFOVAL2, 0x8000);
		emuxki_write(sc, i, EMU_CHAN_LFOVAL1, 0x8000);
		emuxki_write(sc, i, EMU_CHAN_ATKHLDV, 0x7f7f);
		emuxki_write(sc, i, EMU_CHAN_ENVVOL, 0);
		emuxki_write(sc, i, EMU_CHAN_ENVVAL, 0x8000);
	}

	/* set digital outputs format */
	spcs = EMU_SPCS_CLKACCY_1000PPM |
	       EMU_SPCS_SAMPLERATE_48 |
	       EMU_SPCS_CHANNELNUM_LEFT |
	       EMU_SPCS_SOURCENUM_UNSPEC |
	       EMU_SPCS_GENERATIONSTATUS |
	       0x00001200 /* Cat code. */ |
	       0x00000000 /* IEC-958 Mode */ |
	       EMU_SPCS_EMPHASIS_NONE |
	       EMU_SPCS_COPYRIGHT;
	emuxki_write(sc, 0, EMU_SPCS0, spcs);
	emuxki_write(sc, 0, EMU_SPCS1, spcs);
	emuxki_write(sc, 0, EMU_SPCS2, spcs);

	if(sc->sc_type & EMUXKI_AUDIGY2) {
		emuxki_write(sc, 0, EMU_A2_SPDIF_SAMPLERATE,
			EMU_A2_SPDIF_UNKNOWN);

		emuxki_writeptr(sc, EMU_A2_PTR, EMU_A2_DATA, EMU_A2_SRCSEL,
			EMU_A2_SRCSEL_ENABLE_SPDIF |
			EMU_A2_SRCSEL_ENABLE_SRCMULTI);

		emuxki_writeptr(sc, EMU_A2_PTR, EMU_A2_DATA, EMU_A2_SRCMULTI,
			EMU_A2_SRCMULTI_ENABLE_INPUT);
	}

	/* page table */
	sc->ptb = dmamem_alloc(sc, EMU_MAXPTE * sizeof(uint32_t));
	if (sc->ptb == NULL) {
		DPRINTF(("ptb allocation error\n"));
		return ENOMEM;
	}
	emuxki_write(sc, 0, EMU_PTB, DMAADDR(sc->ptb));

	emuxki_write(sc, 0, EMU_TCBS, 0);	/* This means 16K TCB */
	emuxki_write(sc, 0, EMU_TCB, 0);	/* No TCB use for now */

	/* Let's play with sound processor */
	emuxki_initfx(sc);

	/* enable interrupt */
	emuxki_writeio_4(sc, EMU_INTE,
		emuxki_readio_4(sc, EMU_INTE) |
		EMU_INTE_VOLINCRENABLE |
		EMU_INTE_VOLDECRENABLE |
		EMU_INTE_MUTEENABLE);	

	if (sc->sc_type & EMUXKI_AUDIGY2) {
		emuxki_writeio_4(sc, EMU_A_IOCFG,
			emuxki_readio_4(sc, EMU_A_IOCFG) |
			EMU_A_IOCFG_GPOUT0);
	}

	/* enable AUDIO bit */
	uint32_t hcfg = EMU_HCFG_AUDIOENABLE |
	       EMU_HCFG_AUTOMUTE;

	if (sc->sc_type & EMUXKI_AUDIGY2) {
		hcfg |= EMU_HCFG_AC3ENABLE_CDSPDIF |
		        EMU_HCFG_AC3ENABLE_GPSPDIF;
	} else if (sc->sc_type & EMUXKI_AUDIGY) {
	} else {
		hcfg |= EMU_HCFG_LOCKTANKCACHE_MASK;
	}
	/* joystick not supported now */
	emuxki_writeio_4(sc, EMU_HCFG, hcfg);

	return 0;
}

/* dsp programming */

static void
emuxki_dsp_addop(struct emuxki_softc *sc, uint16_t *pc, uint8_t op,
		  uint16_t r, uint16_t a, uint16_t x, uint16_t y)
{
	uint32_t loword;
	uint32_t hiword;
	int reg;

	if (sc->sc_type & EMUXKI_AUDIGY) {
		reg = EMU_A_MICROCODEBASE;
		loword = (x << 12) & EMU_A_DSP_LOWORD_OPX_MASK;
		loword |= y & EMU_A_DSP_LOWORD_OPY_MASK;
		hiword = (op << 24) & EMU_A_DSP_HIWORD_OPCODE_MASK;
		hiword |= (r << 12) & EMU_A_DSP_HIWORD_RESULT_MASK;
		hiword |= a & EMU_A_DSP_HIWORD_OPA_MASK;
	} else {
		reg = EMU_MICROCODEBASE;
		loword = (x << 10) & EMU_DSP_LOWORD_OPX_MASK;
		loword |= y & EMU_DSP_LOWORD_OPY_MASK;
		hiword = (op << 20) & EMU_DSP_HIWORD_OPCODE_MASK;
		hiword |= (r << 10) & EMU_DSP_HIWORD_RESULT_MASK;
		hiword |= a & EMU_DSP_HIWORD_OPA_MASK;
	}

	reg += (*pc) * 2;
	/* must ordering; lo, hi */
	emuxki_write(sc, 0, reg, loword);
	emuxki_write(sc, 0, reg + 1, hiword);

	(*pc)++;
}

static void
emuxki_initfx(struct emuxki_softc *sc)
{
	uint16_t pc;

	/* Set all GPRs to 0 */
	for (pc = 0; pc < 256; pc++)
		emuxki_write(sc, 0, EMU_DSP_GPR(pc), 0);
	for (pc = 0; pc < 160; pc++) {
		emuxki_write(sc, 0, EMU_TANKMEMDATAREGBASE + pc, 0);
		emuxki_write(sc, 0, EMU_TANKMEMADDRREGBASE + pc, 0);
	}

	/* stop DSP, single step mode */
	emuxki_write(sc, 0, X1(DBG), X1(DBG_SINGLE_STEP));

	/* XXX: delay (48kHz equiv. 21us) if needed */

	/* start DSP programming */
	pc = 0;

	/* OUT[L/R] = 0 + FX[L/R] * 1 */
	emuxki_dsp_addop(sc, &pc, EMU_DSP_OP_MACINTS,
		X2(DSP_OUTL, DSP_OUT_A_FRONT),
		X1(DSP_CST(0)),
		X1(DSP_FX(0)),
		X1(DSP_CST(4)));
	emuxki_dsp_addop(sc, &pc, EMU_DSP_OP_MACINTS,
		X2(DSP_OUTR, DSP_OUT_A_FRONT),
		X1(DSP_CST(0)),
		X1(DSP_FX(1)),
		X1(DSP_CST(4)));
#if 0
	/* XXX: rear feature??? */
	/* Rear OUT[L/R] = 0 + FX[L/R] * 1 */
	emuxki_dsp_addop(sc, &pc, EMU_DSP_OP_MACINTS,
		X2(DSP_OUTL, DSP_OUT_A_REAR),
		X1(DSP_CST(0)),
		X1(DSP_FX(0)),
		X1(DSP_CST(1)));
	emuxki_dsp_addop(sc, &pc, EMU_DSP_OP_MACINTS,
		X2(DSP_OUTR, DSP_OUT_A_REAR),
		X1(DSP_CST(0)),
		X1(DSP_FX(1)),
		X1(DSP_CST(1)));
#endif
	/* ADC recording[L/R] = AC97 In[L/R] */
	emuxki_dsp_addop(sc, &pc, EMU_DSP_OP_ACC3,
		X2(DSP_OUTL, DSP_OUT_ADC),
		X2(DSP_INL, DSP_IN_AC97),
		X1(DSP_CST(0)),
		X1(DSP_CST(0)));
	emuxki_dsp_addop(sc, &pc, EMU_DSP_OP_ACC3,
		X2(DSP_OUTR, DSP_OUT_ADC),
		X2(DSP_INR, DSP_IN_AC97),
		X1(DSP_CST(0)),
		X1(DSP_CST(0)));

	/* fill NOP the rest of the microcode */
	while (pc < 512) {
		emuxki_dsp_addop(sc, &pc, EMU_DSP_OP_ACC3,
			X1(DSP_CST(0)),
			X1(DSP_CST(0)),
			X1(DSP_CST(0)),
			X1(DSP_CST(0)));
	}

	/* clear single step flag, run DSP */
	emuxki_write(sc, 0, X1(DBG), 0);
}

/*
 * operations
 */

static void
emuxki_play_start(struct emuxki_softc *sc, int ch,
    uint32_t start, uint32_t end)
{
	uint32_t pitch;
	uint32_t volume;

	/* 48kHz:16384 = 128/375 */
	pitch = sc->play.sample_rate * 128 / 375;
	volume = 32767;

	emuxki_write(sc, ch, EMU_CHAN_DSL,
		(0 << 24) |	/* send amound D = 0 */
		end);

	emuxki_write(sc, ch, EMU_CHAN_PSST,
		(0 << 24) | 	/* send amount C = 0 */
		start);

	emuxki_write(sc, ch, EMU_CHAN_VTFT,
		(volume << 16) | 
		(0xffff));	/* cutoff filter = none */

	emuxki_write(sc, ch, EMU_CHAN_CVCF,
		(volume << 16) |
		(0xffff));	/* cutoff filter = none */
	
	emuxki_write(sc, ch, EMU_CHAN_PTRX,
		(pitch << 16) | 
		((ch == 0 ? 0x7f : 0) << 8) |	/* send amount A = 255,0(L) */
		((ch == 0 ? 0 : 0x7f)));		/* send amount B = 0,255(R) */

	/* set the pitch to start */
	emuxki_write(sc, ch, EMU_CHAN_CPF,
		(pitch << 16) |
		EMU_CHAN_CPF_STEREO_MASK);	/* stereo only */
}

static void
emuxki_play_stop(struct emuxki_softc *sc, int ch)
{
	/* pitch = 0 to stop playing */
	emuxki_write(sc, ch, EMU_CHAN_CPF, EMU_CHAN_CPF_STOP_MASK);
	/* volume = 0 */
	emuxki_write(sc, ch, EMU_CHAN_CVCF, 0);
}

static void
emuxki_timer_start(struct emuxki_softc *sc, int request)
{
	uint32_t timer;

	if (sc->timer == 0) {
		/* frame count of half PTE at 48kHz */
		timer = EMU_PTESIZE / 4 / 2;

		/* EMU_TIMER is 16bit register */
		emuxki_writeio_2(sc, EMU_TIMER, timer);
		emuxki_writeio_4(sc, EMU_INTE,
			emuxki_readio_4(sc, EMU_INTE) |
			EMU_INTE_INTERTIMERENB);
		DPRINTF(("timer start\n"));
	}
	sc->timer |= request;
}

static void
emuxki_timer_stop(struct emuxki_softc *sc, int request)
{
	sc->timer &= ~request;

	if (sc->timer == 0) {
		emuxki_writeio_4(sc, EMU_INTE,
			emuxki_readio_4(sc, EMU_INTE) &
			~EMU_INTE_INTERTIMERENB);
		/* EMU_TIMER is 16bit register */
		emuxki_writeio_2(sc, EMU_TIMER, 0);
		DPRINTF(("timer stop\n"));
	}
}

/*
 * audio interface
 */

static int
emuxki_open(void *addr, int flags)
{
	struct emuxki_softc *sc = addr;

	DPRINTF(("%s for %s%s\n", __func__,
		flags & FWRITE ? "P" : "",
		flags & FREAD ? "R" : ""));

	if (sc->mode) {
		DPRINTF(("dup open"));
		return EINVAL;
	}

	if (flags & FWRITE) {
		sc->mode |= AUMODE_PLAY;
	}
	if (flags & FREAD) {
		sc->mode |= AUMODE_RECORD;
	}
	return 0;
}

static void
emuxki_close(void *addr)
{
	struct emuxki_softc *sc = addr;

	DPRINTF(("%s\n", __func__));

	sc->mode = 0;
}

static int
emuxki_query_format(void *addr, audio_format_query_t *afp)
{
	return audio_query_format(emuxki_formats, EMUXKI_NFORMATS, afp);
}

static int
emuxki_set_format(void *addr, int setmode,
	const audio_params_t *play, const audio_params_t *rec,
	audio_filter_reg_t *pfil, audio_filter_reg_t *rfil)
{
	struct emuxki_softc *sc = addr;

	/* XXX always success */
	if (play) {
		sc->play = *play;
	}

	if (rec) {
		sc->rec = *rec;
	}
	return 0;
}

static int
emuxki_halt_output(void *addr)
{
	struct emuxki_softc *sc = addr;

	emuxki_timer_stop(sc, EMU_TIMER_PLAY);

	emuxki_play_stop(sc, 0);
	emuxki_play_stop(sc, 1);
	return 0;

}

static int
emuxki_halt_input(void *addr)
{
	DPRINTF(("not supported now\n"));
	return 0;
}

static int
emuxki_intr(void *arg)
{
	struct emuxki_softc *sc = arg;
	uint32_t ipr;

	mutex_spin_enter(&sc->sc_intr_lock);

	ipr = emuxki_readio_4(sc, EMU_IPR);
	DPRINTFN(3, ("emuxki: ipr=%08x\n", ipr));
	if (ipr & EMU_IPR_INTERVALTIMER) {
		if (sc->pintr) {
			uint32_t curaddr;
			/* read ch 0 */
			curaddr = emuxki_read(sc, 0, EMU_CHAN_CCCA) &
				EMU_CHAN_CCCA_CURRADDR_MASK;
			DPRINTFN(3, ("curaddr=%08x\n", curaddr));
			curaddr *= sc->pframesize;

			if (curaddr < sc->poffset) curaddr += sc->plength;
			if (curaddr >= sc->poffset + sc->pblksize) {
				dmamem_sync(sc->pmem, BUS_DMASYNC_POSTWRITE);
				sc->pintr(sc->pintrarg);
				sc->poffset += sc->pblksize;
				if (sc->poffset >= sc->plength) {
					sc->poffset -= sc->plength;
				}
				dmamem_sync(sc->pmem, BUS_DMASYNC_PREWRITE);
			}
		}
		if (sc->rintr) {
			/* not impl */
		}
	}
#if defined(EMUXKI_DEBUG)
 	else {
		char buf[1024];
		buf[0] = '\0';

		if (ipr & EMU_IPR_RATETRCHANGE)
			strcat(buf, " RATETRCHANGE");
		if (ipr & EMU_IPR_FXDSP)
			strcat(buf, " FXDSP");
		if (ipr & EMU_IPR_FORCEINT)
			strcat(buf, " FORCEINT");
		if (ipr & EMU_PCIERROR)
			strcat(buf, " PCIERROR");
		if (ipr & EMU_IPR_VOLINCR)
			strcat(buf, " VOLINCR");
		if (ipr & EMU_IPR_VOLDECR)
			strcat(buf, " VOLDECR");
		if (ipr & EMU_IPR_MUTE)
			strcat(buf, " MUTE");
		if (ipr & EMU_IPR_MICBUFFULL)
			strcat(buf, " MICBUFFULL");
		if (ipr & EMU_IPR_MICBUFHALFFULL)
			strcat(buf, " MICBUFHALFFULL");
		if (ipr & EMU_IPR_ADCBUFFULL)
			strcat(buf, " ADCBUFFULL");
		if (ipr & EMU_IPR_ADCBUFHALFFULL)
			strcat(buf, " ADCBUFHALFFULL");
		if (ipr & EMU_IPR_EFXBUFFULL)
			strcat(buf, " EFXBUFFULL");
		if (ipr & EMU_IPR_EFXBUFHALFFULL)
			strcat(buf, " EFXBUFHALFFULL");
		if (ipr & EMU_IPR_GPSPDIFSTCHANGE)
			strcat(buf, " GPSPDIFSTCHANGE");
		if (ipr & EMU_IPR_CDROMSTCHANGE)
			strcat(buf, " CDROMSTCHANGE");
		if (ipr & EMU_IPR_INTERVALTIMER)
			/* nop */;
		if (ipr & EMU_IPR_MIDITRANSBUFE)
			strcat(buf, " MIDITRANSBUFE");
		if (ipr & EMU_IPR_MIDIRECVBUFE)
			strcat(buf, " MIDIRECVBUFE");
		if (ipr & EMU_IPR_CHANNELLOOP)
			strcat(buf, " CHANNELLOOP");

		DPRINTF(("unexpected intr: %s", buf));
	}
#endif

	emuxki_writeio_4(sc, EMU_IPR, ipr);

	mutex_spin_exit(&sc->sc_intr_lock);

	/* handled */
	return 1;
}

static int
emuxki_getdev(void *addr, struct audio_device *dev)
{
	struct emuxki_softc *sc = addr;

	*dev = sc->sc_audv;
	return 0;
}

static int
emuxki_set_port(void *addr, mixer_ctrl_t *mctl)
{
	struct emuxki_softc *sc = addr;

	return sc->codecif->vtbl->mixer_set_port(sc->codecif, mctl);
}

static int
emuxki_get_port(void *addr, mixer_ctrl_t *mctl)
{
	struct emuxki_softc *sc = addr;

	return sc->codecif->vtbl->mixer_get_port(sc->codecif, mctl);
}

static int
emuxki_query_devinfo(void *addr, mixer_devinfo_t *minfo)
{
	struct emuxki_softc *sc = addr;

	return sc->codecif->vtbl->query_devinfo(sc->codecif, minfo);
}

static void *
emuxki_allocm(void *addr, int direction, size_t size)
{
	struct emuxki_softc *sc = addr;

	if (direction == AUMODE_PLAY) {
		if (sc->pmem) {
			DPRINTF(("%s: realloc not supported\n", __func__));
			return NULL;
		}
		sc->pmem = dmamem_alloc(sc, size);
		return KERNADDR(sc->pmem);
	} else {
		if (sc->rmem) {
			DPRINTF(("%s: realloc not supported\n", __func__));
			return NULL;
		}		
		sc->rmem = dmamem_alloc(sc, size);
		return KERNADDR(sc->rmem);
	}
}

static void
emuxki_freem(void *addr, void *ptr, size_t size)
{
	struct emuxki_softc *sc = addr;

	if (sc->pmem && ptr == KERNADDR(sc->pmem)) {
		dmamem_free(sc->pmem);
		sc->pmem = NULL;
	}
	if (sc->rmem && ptr == KERNADDR(sc->rmem)) {
		dmamem_free(sc->rmem);
		sc->rmem = NULL;
	}
}

/*
 * blocksize rounding to EMU_PTESIZE. It is for easy to drive.
 */
static int
emuxki_round_blocksize(void *addr, int blksize,
    int mode, const audio_params_t* param)
{
	return roundup(blksize, EMU_PTESIZE);
}

static size_t
emuxki_round_buffersize(void *addr, int direction, size_t bsize)
{
	return roundup(bsize, EMU_PTESIZE);
}

static int
emuxki_get_props(void *addr)
{
	return AUDIO_PROP_INDEPENDENT | AUDIO_PROP_FULLDUPLEX;
}

static int
emuxki_trigger_output(void *addr, void *start, void *end, int blksize,
    void (*inth) (void *), void *inthparam, const audio_params_t *params)
{
	struct emuxki_softc *sc;
	int npage;
	uint32_t *kptb;
	bus_addr_t dpmem;
	int i;
	uint32_t hwstart;
	uint32_t hwend;
	
	sc = addr;

	if (sc->pmem == NULL) {
		DPRINTF(("pmem == NULL\n"));
		return ENXIO;
	}

	if (start != KERNADDR(sc->pmem)) {
		DPRINTF(("start != KERNADDR(sc->pmem)\n"));
		return ENXIO;
	}

	sc->pframesize = 4;	/* channels * bit / 8 = 2*16/8=4 */
	sc->pblksize = blksize;
	sc->plength = (char *)end - (char *)start;
	sc->poffset = 0;
	npage = (sc->plength + EMU_PTESIZE - 1) / EMU_PTESIZE;

	kptb = KERNADDR(sc->ptb);
	dpmem = DMAADDR(sc->pmem);
	for (i = 0; i < npage; i++) {
		kptb[i] = htole32(dpmem << 1);
		dpmem += EMU_PTESIZE;
	}
	dmamem_sync(sc->ptb, BUS_DMASYNC_PREWRITE);

	hwstart = 0;
	hwend = hwstart + sc->plength / sc->pframesize;

	sc->pintr = inth;
	sc->pintrarg = inthparam;

	dmamem_sync(sc->pmem, BUS_DMASYNC_PREWRITE);

	emuxki_play_start(sc, 0, hwstart, hwend);
	emuxki_play_start(sc, 1, hwstart, hwend);

	emuxki_timer_start(sc, EMU_TIMER_PLAY);

	return 0;
}

static int
emuxki_trigger_input(void *addr, void *start, void *end, int blksize,
    void (*inth) (void *), void *inthparam, const audio_params_t *params)
{
	DPRINTF(("not implemented\n"));
	return 0;
}

static void
emuxki_get_locks(void *arg, kmutex_t **intr, kmutex_t **proc)
{
	struct emuxki_softc *sc;

	sc = arg;
	*intr = &sc->sc_intr_lock;
	*proc = &sc->sc_lock;
}

/*
 * AC97
 */

static int
emuxki_ac97_init(struct emuxki_softc *sc)
{
	sc->hostif.arg = sc;
	sc->hostif.attach = emuxki_ac97_attach;
	sc->hostif.read = emuxki_ac97_read;
	sc->hostif.write = emuxki_ac97_write;
	sc->hostif.reset = emuxki_ac97_reset;
	sc->hostif.flags = emuxki_ac97_flags;
	return ac97_attach(&sc->hostif, sc->sc_dev, &sc->sc_lock);
}

/*
 * AC97 callbacks
 */

static int
emuxki_ac97_attach(void *arg, struct ac97_codec_if *codecif)
{
	struct emuxki_softc *sc;

	sc = arg;
	sc->codecif = codecif;
	return 0;
}

static int
emuxki_ac97_read(void *arg, uint8_t reg, uint16_t *val)
{
	struct emuxki_softc *sc;

	sc = arg;
	mutex_spin_enter(&sc->sc_index_lock);
	emuxki_writeio_1(sc, EMU_AC97ADDR, reg);
	*val = emuxki_readio_2(sc, EMU_AC97DATA);
	mutex_spin_exit(&sc->sc_index_lock);

	return 0;
}

static int
emuxki_ac97_write(void *arg, uint8_t reg, uint16_t val)
{
	struct emuxki_softc *sc;

	sc = arg;
	mutex_spin_enter(&sc->sc_index_lock);
	emuxki_writeio_1(sc, EMU_AC97ADDR, reg);
	emuxki_writeio_2(sc, EMU_AC97DATA, val);
	mutex_spin_exit(&sc->sc_index_lock);

	return 0;
}

static int
emuxki_ac97_reset(void *arg)
{

	return 0;
}

static enum ac97_host_flags
emuxki_ac97_flags(void *arg)
{

	return AC97_HOST_SWAPPED_CHANNELS;
}

