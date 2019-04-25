
/*
 * PCI
 */

#define	EMU_PCI_CBIO		0x10
#define EMU_SUBSYS_APS		0x40011102

#define	EMU_PTESIZE		4096
// XXX test for limit 256 pages
#define EMU_MAXPTE	0x100
#define EMU_NUMCHAN	64
/*
 * DMA memory management
 */

#define	EMU_DMA_ALIGN	4096
#define	EMU_DMA_NSEGS	1

struct dmamem {
	bus_dma_tag_t   dmat;
	bus_size_t      size;
	bus_size_t      align;
	bus_size_t      bound;
	bus_dma_segment_t *segs;
	int		nsegs;
	int		rsegs;
	void *		kaddr;
	bus_dmamap_t    map;
};

#define	KERNADDR(ptr)		((void *)((ptr)->kaddr))
#define	DMASEGADDR(ptr, segno)	((ptr)->segs[segno].ds_addr)
#define	DMAADDR(ptr)		DMASEGADDR(ptr, 0)
#define DMASIZE(ptr)		((ptr)->size)


struct emuxki_softc {
	device_t	sc_dev;
	audio_device_t	sc_audv;
	enum {
		EMUXKI_SBLIVE = 0x00, EMUXKI_AUDIGY = 0x01,
		EMUXKI_AUDIGY2 = 0x02, EMUXKI_LIVE_5_1 = 0x04,
		EMUXKI_APS = 0x08
	} sc_type;

	/* Autoconfig parameters */
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	//bus_space_tag_t		memt;
	//bus_space_handle_t	memh;
	bus_addr_t		sc_iob;
	bus_size_t		sc_ios;
	pci_chipset_tag_t	sc_pc;		/* PCI tag */
	bus_dma_tag_t		sc_dmat;
	void			*sc_ih;		/* interrupt handler */
	kmutex_t		sc_intr_lock;
	kmutex_t		sc_lock;
	kmutex_t		sc_index_lock;

	/* register parameters */
	struct dmamem		*ptb;		/* page table */

	/* audio interface parameters */
	int mode;				/* play and/or rec */

	struct dmamem		*pmem;		/* play memory */
	void (*pintr)(void *);
	void *pintrarg;
	audio_params_t play;
	int pframesize;
	int pblksize;
	int plength;
	int poffset;

	struct dmamem		*rmem;		/* rec memory */
	void (*rintr)(void *);
	void *rintrarg;
	audio_params_t rec;

	int timer;
#define EMU_TIMER_PLAY	0x01
#define EMU_TIMER_REC	0x02

	/* others */

	struct ac97_host_if	hostif;
	struct ac97_codec_if	*codecif;
	device_t		sc_audev;

};
