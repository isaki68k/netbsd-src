// vi:set ts=8:
#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/endian.h>
#include <sys/kmem.h>

#include <sys/cpu.h>
#include <sys/audioio.h>
#include <dev/audio_if.h>

#include <machine/autoconf.h>

#include <luna68k/dev/psgpamvar.h>
#include <luna68k/dev/xpcommon.h>
#include <luna68k/luna68k/isr.h>

#include "pam2tbl.c"
#include "pam3tbl.c"

#include "firmware.c"

// デバッグレベルは
// 1: open/close/set_param等
// 2: read/write/ioctlシステムコールくらいまでは含む
// 3: 割り込み以外のTRACEも含む
// 4: 割り込み内のTRACEも含む (要 AUDIO_DEBUG_MLOG)
#define AUDIO_DEBUG	0

// デバッグ用なんちゃってメモリログ。
#define AUDIO_DEBUG_MLOG

#if defined(AUDIO_DEBUG_MLOG)
#if defined(_KERNEL)
#include <dev/audio/mlog.h>
#else
#include "mlog.h"
#endif
#else
#define audio_mlog_flush()	/**/
#endif

#ifdef AUDIO_DEBUG
#define DPRINTF(n, fmt...)	do {	\
	if (audiodebug >= (n)) {	\
		if (cpu_intr_p()) {	\
			audio_mlog_printf(fmt);	\
		} else {	\
			audio_mlog_flush();	\
			printf(fmt);		\
		}	\
	}				\
} while (0)
static int	audiodebug = AUDIO_DEBUG;
#else
#define DPRINTF(n, fmt...)	do { } while (0)
#endif
// select XPENC
#define USE_XPENC	XPENC_PAM2_19

#if USE_XPENC == XPENC_PAM2_0
	// XPCPUFREQ / (156 + 26 * T)
#define XP_STR_ENC	"2-phase 0 wait"
#define XP_STR_PAMFREQ	"139.636kHz-236.308kHz"
#define XP_TIMER_CON	156
#define XP_TIMER_MUL	26
#define XP_SAMPLE_RATE_COUNT	2
#define XP_SAMPLE_RATE	39384, 8149
#define XP_STRIDE	2
#define XP_FILTER	psgpam_internal_to_pam2

#elif USE_XPENC == XPENC_PAM2_9
	// XPCPUFREQ / (132 + 44 * T)
#define XP_STR_ENC	"2-phase 9 wait"
#define XP_STR_PAMFREQ	"139.636kHz"
#define XP_TIMER_CON	132
#define XP_TIMER_MUL	44
#define XP_SAMPLE_RATE_COUNT	2
#define XP_SAMPLE_RATE	46545, 8214
#define XP_STRIDE	2
#define XP_FILTER	psgpam_internal_to_pam2

#elif USE_XPENC == XPENC_PAM2_12
	// XPCPUFREQ / (100 + 50 * T)
	// skip: 61440
#define XP_STR_ENC	"2-phase 12 wait"
#define XP_STR_PAMFREQ	"122.88kHz"
#define XP_TIMER_CON	100
#define XP_TIMER_MUL	50
#define XP_SAMPLE_RATE_COUNT	1
#define XP_SAMPLE_RATE	40960
#define XP_STRIDE	2
#define XP_FILTER	psgpam_internal_to_pam2

#elif USE_XPENC == XPENC_PAM2_19
	// XPCPUFREQ / (64 + 64 * T)
	// skip: 96000
#define XP_STR_ENC	"2-phase 19 wait"
#define XP_STR_PAMFREQ	"96kHz"
#define XP_TIMER_CON	64
#define XP_TIMER_MUL	64
#define XP_SAMPLE_RATE_COUNT	2
#define XP_SAMPLE_RATE	48000, 8000
#define XP_STRIDE	2
#define XP_FILTER	psgpam_internal_to_pam2

#elif USE_XPENC == XPENC_PAM3_0
	// XPCPUFREQ / (114 + 39 * T)
	// skip: 53895
#define XP_STR_ENC	"3-phase 0 wait"
#define XP_STR_PAMFREQ	"97.524kHz-157.538kHz"
#define XP_TIMER_CON	114
#define XP_TIMER_MUL	39
#define XP_SAMPLE_RATE_COUNT	1
#define XP_SAMPLE_RATE	40157
#define XP_STRIDE	4
#define XP_FILTER	psgpam_internal_to_pam3

#elif USE_XPENC == XPENC_PAM3_12
	// XPCPUFREQ / (150 + 75 * T)
#define XP_STR_ENC	"3-phase 12 wait"
#define XP_STR_PAMFREQ	"81.92kHz"
#define XP_TIMER_CON	150
#define XP_TIMER_MUL	75
#define XP_SAMPLE_RATE_COUNT	1
#define XP_SAMPLE_RATE	40960
#define XP_STRIDE	4
#define XP_FILTER	psgpam_internal_to_pam3

#elif USE_XPENC == XPENC_PAM4_12
	// XPCPUFREQ / (200 + 100 * T)
#define XP_STR_ENC	"4-phase 12 wait"
#define XP_STR_PAMFREQ	"61.44kHz"
#define XP_TIMER_CON	200
#define XP_TIMER_MUL	100
#define XP_SAMPLE_RATE_COUNT	1
#define XP_SAMPLE_RATE	30720
#define XP_STRIDE	4
#define XP_FILTER	psgpam_internal_to_pam4

#else
#error Error: PAM encoding not selected
#endif

#define XP_BUFTOP_INITIAL	0x4000
#define XP_ATN_STAT	0x80808080
#define XP_ATN_RELOAD	0xc0c0c0c0
#define XP_ATN_RESET	0xe0e0e0e0

struct psgpam_softc {
	device_t sc_dev;
	vaddr_t sc_shm_base;
	vsize_t sc_shm_size;

	void (*sc_intr)(void *);
	void *sc_arg;

	kmutex_t sc_intr_lock;
	kmutex_t sc_thread_lock;

	int      sc_started;
	int      sc_outcount;
	int      sc_xp_state;	//
	uint16_t sc_xp_addr;	// XP buffer addr
	int      sc_xp_enc;
	int      sc_xp_timer;
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
static int  psgpam_halt_output(void *);
static int  psgpam_getdev(void *, struct audio_device *);
static int  psgpam_set_port(void *, mixer_ctrl_t *);
static int  psgpam_get_port(void *, mixer_ctrl_t *);
static int  psgpam_query_devinfo(void *, mixer_devinfo_t *);
static int  psgpam_get_props(void *);
static void psgpam_get_locks(void *, kmutex_t **, kmutex_t **);

static int  psgpam_intr(void *);
static void psgpam_internal_to_pam2(audio_filter_arg_t *);
static void psgpam_internal_to_pam3(audio_filter_arg_t *);
static void psgpam_internal_to_pam4(audio_filter_arg_t *);

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
	.halt_output		= psgpam_halt_output,
	.getdev			= psgpam_getdev,
	.set_port		= psgpam_set_port,
	.get_port		= psgpam_get_port,
	.query_devinfo		= psgpam_query_devinfo,
	.get_props		= psgpam_get_props,
	.get_locks		= psgpam_get_locks,
};

static struct audio_device psgpam_device = {
	"PSG PAM",
	"0.1",
	"",
};

static const struct audio_format psgpam_formats[] = {
	{ NULL, AUMODE_PLAY, AUDIO_ENCODING_SLINEAR_BE, 16, 16,
	  1, AUFMT_MONAURAL,
	  XP_SAMPLE_RATE_COUNT, { XP_SAMPLE_RATE },
	},
};

/* private functions */
static
void
psgpam_xp_start(struct psgpam_softc *sc)
{
	DPRINTF(3, "XP starting..");
	if (xp_readmem8(XP_STAT_PLAY) == 1) {
		DPRINTF(1, "XP already started???");
	}

	xp_writemem8(XP_TIMER, sc->sc_xp_timer);
	xp_writemem8(XP_ENC, sc->sc_xp_enc);
	xp_writemem16le(XP_BUFTOP, XP_BUFTOP_INITIAL);
	DPRINTF(3, "XPENC=%d XPTIMER=%d\n", sc->sc_xp_enc, sc->sc_xp_timer);
	DPRINTF(3, " BUFTOP=%04X ", xp_readmem16le(XP_BUFTOP));

	while (xp_readmem8(XP_STAT_READY) != 1) {
	}

	xp_intr5_enable();

	xp_writemem8(XP_CMD_START, 1);

	while (xp_readmem8(XP_STAT_PLAY) != 1) {
	}

	DPRINTF(3, "XP started\n");
}

/* MI MD API */

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
	struct mainbus_attach_args *maa = aux;

#if defined(AUDIO_DEBUG_MLOG)
	audio_mlog_init();
#endif

	sc = device_private(self);
	sc->sc_dev = self;

	aprint_normal(": HD647180X I/O processor as PSG PAM\n");
	aprint_normal(": PSGPAM encoding = " XP_STR_ENC "\n");
	aprint_normal(": PAM frequency = " XP_STR_PAMFREQ "\n");

	sc->sc_shm_base = XP_SHM_BASE;
	sc->sc_shm_size = XP_SHM_SIZE;

	mutex_init(&sc->sc_thread_lock, MUTEX_DEFAULT, IPL_NONE);
	mutex_init(&sc->sc_intr_lock, MUTEX_DEFAULT, IPL_SCHED);

	isrlink_autovec(psgpam_intr, sc, maa->ma_ilvl, ISRPRI_TTYNOBUF);

	audio_attach_mi(&psgpam_hw_if, sc, sc->sc_dev);
}

void psgpam_detatch(void);
void
psgpam_detatch(void)
{
	// STUB
#if defined(AUDIO_DEBUG_MLOG)
	audio_mlog_free();
#endif
}

static int
psgpam_open(void *hdl, int flags)
{
	struct psgpam_softc *sc;

	DPRINTF(1, "%s: flags=0x%x\n", __func__, flags);
	sc = hdl;

	// firmware transfer
	xp_cpu_reset_hold();
	delay(100);
	memcpy((void *)sc->sc_shm_base, xp_builtin_firmware, xp_firmware_len);
	// XXX いるの?
	delay(100);
	xp_cpu_reset_release();

	// TODO: /dev/xp との排他制御とか関係性とか。

	sc->sc_xp_state = 0;
	sc->sc_started = 0;
	sc->sc_outcount = 0;

	return 0;
}

static void
psgpam_close(void *hdl)
{
	xp_intr5_disable();

	// force stop
	xp_cpu_reset();

	DPRINTF(1, "%s\n", __func__);
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

	DPRINTF(1, "%s\n", __func__);

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
	struct psgpam_softc *sc;

	sc = hdl;
	DPRINTF(1, "%s: mode=%d %s/%dbit/%dch/%dHz\n", __func__,
	    setmode, audio_encoding_name(play->encoding),
	    play->precision, play->channels, play->sample_rate);

	/* *play and *rec are identical because !AUDIO_PROP_INDEPENDENT */

	sc->sc_xp_enc = USE_XPENC;

	sc->sc_xp_timer = (XP_CPU_FREQ / play->sample_rate - XP_TIMER_CON)
		/ XP_TIMER_MUL;
	if (sc->sc_xp_timer < 0) sc->sc_xp_timer = 0;

	DPRINTF(1, "XPENC=%d XPTIMER=%d\n", sc->sc_xp_enc, sc->sc_xp_timer);

	// set filter
	pfil->codec = XP_FILTER;
	// XXX AUDIO_ENCODING_PSGPAM2,3,4
	pfil->param.encoding = AUDIO_ENCODING_NONE;
	pfil->param.precision = XP_STRIDE * 8;
	pfil->param.validbits = XP_STRIDE * 8;

	return 0;
}
#else
static int
psgpam_set_params(void *hdl, int setmode, int usemode,
	audio_params_t *play, audio_params_t *rec,
	stream_filter_list_t *pfil, stream_filter_list_t *rfil)
{

	DPRINTF(1, "%s setmode=%x usemode=%x\n", __func__, setmode, usemode);
	return 0;
}
#endif

static int
psgpam_start_output(void *hdl, void *block, int blksize,
	void (*intr)(void *), void *intrarg)
{
	int markoffset;
	uint32_t marker;

	struct psgpam_softc *sc;

	sc = hdl;

	DPRINTF(2, "%s block=%p blksize=%d\n", __func__, block, blksize);

	sc->sc_outcount++;

	sc->sc_intr = intr;
	sc->sc_arg = intrarg;

	markoffset = blksize - XP_STRIDE;

	if (sc->sc_xp_state == 0) {
		marker = XP_ATN_STAT;
		sc->sc_xp_addr = XP_BUFTOP_INITIAL;

		if (sc->sc_started == 0) {
			// if first transfer, interrupt at top of buffer.
			markoffset = 0;
		}
	} else {
		marker = XP_ATN_RELOAD;
	}

	// marking
#if XP_STRIDE == 2
	uint16_t *markptr = (uint16_t*)((uint8_t*)block + markoffset);
#else // STRIDE == 4
	uint32_t *markptr = (uint32_t*)((uint8_t*)block + markoffset);
#endif
	*markptr |= marker;

	// transfer
	memcpy(
		xp_shmptr(sc->sc_xp_addr),
		block,
		blksize);

	DPRINTF(2, "check: %04X %02X\n",
		sc->sc_xp_addr + markoffset,
		xp_readmem16le(sc->sc_xp_addr + markoffset));

	sc->sc_xp_addr += blksize;

	// invert state
	sc->sc_xp_state = ~sc->sc_xp_state;

	// play start
	if (sc->sc_started == 0) {
		// 先にフラグを立てておく
		sc->sc_started = 1;
		psgpam_xp_start(sc);
	}

	return 0;
}

static int
psgpam_halt_output(void *hdl)
{
	struct psgpam_softc *sc = hdl;

	DPRINTF(2, "%s\n", __func__);

	xp_intr5_disable();

	// XP reset
	xp_cpu_reset();

	sc->sc_started = 0;
	sc->sc_xp_state = 0;

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
	DPRINTF(2, "%s\n", __func__);
	return 0;
}

static int
psgpam_get_port(void *hdl, mixer_ctrl_t *mc)
{
	DPRINTF(2, "%s\n", __func__);
	return 0;
}

static int
psgpam_query_devinfo(void *hdl, mixer_devinfo_t *di)
{

	DPRINTF(2, "%s %d\n", __func__, di->index);
	switch (di->index) {
	default:
		return EINVAL;
	}
	return 0;
}

static int
psgpam_get_props(void *hdl)
{
	return AUDIO_PROP_PLAYBACK;
}

static void
psgpam_get_locks(void *hdl, kmutex_t **intr, kmutex_t **thread)
{
	struct psgpam_softc *sc = hdl;

	*intr = &sc->sc_intr_lock;
	*thread = &sc->sc_thread_lock;
}

static int
psgpam_intr(void *hdl)
{
	struct psgpam_softc *sc = hdl;

	xp_intr5_acknowledge();
	DPRINTF(4, "psgpam intr\n");

	if (sc->sc_outcount <= 0) {
		DPRINTF(4, "loop detected");
		psgpam_halt_output(sc);
		return 1;
	}
	sc->sc_outcount--;

	mutex_spin_enter(&sc->sc_intr_lock);

	if (sc->sc_intr) {
		sc->sc_intr(sc->sc_arg);
	} else {
		DPRINTF(1, "psgpam_intr: spurious interrupt\n");
	}

	mutex_spin_exit(&sc->sc_intr_lock);

	/* handled */
	return 1;
}

void __unused
psgpam_internal_to_pam2(audio_filter_arg_t *arg)
{
	const aint_t *s = arg->src;
	uint16_t *d = arg->dst;

	const uint16_t *table = PAM2_TABLE;

	DPRINTF(4, "internal_to_pam2: arg->src=%p arg->dst=%p",
		arg->src, arg->dst);

	for (int i = 0; i < arg->count; i++) {
		uint8_t x = ((*s++) >> (AUDIO_INTERNAL_BITS - 8)) ^ 0x80;
		*d++ = htobe16(table[x]);
	}
}

void __unused
psgpam_internal_to_pam3(audio_filter_arg_t *arg)
{
	const aint_t *s = arg->src;
	uint32_t *d = arg->dst;

	const uint32_t *table = PAM3_TABLE;

	for (int i = 0; i < arg->count; i++) {
		uint8_t x = ((*s++) >> (AUDIO_INTERNAL_BITS - 8)) ^ 0x80;
		*d++ = htobe32(table[x]);
	}
}

void __unused
psgpam_internal_to_pam4(audio_filter_arg_t *arg)
{
#if 0
	const aint_t *s = arg->src;
	uint32_t *d = arg->dst;

	const uint32_t *table = PAM4_TABLE;

	for (int i = 0; i < arg->count; i++) {
		uint8_t x = ((*s++) >> (AUDIO_INTERNAL_BITS - 8)) ^ 0x80;
		*d++ = htobe32(table[x]);
	}
#endif
}
