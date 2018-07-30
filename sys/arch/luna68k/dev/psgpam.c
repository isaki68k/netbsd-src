// vi:set ts=8:
#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/endian.h>
#include <sys/kmem.h>
#include <sys/sysctl.h>

#include <sys/cpu.h>
#include <sys/audioio.h>
#include <dev/audio_if.h>

#include <machine/autoconf.h>

#include <luna68k/dev/psgpamvar.h>
#include <luna68k/dev/xpbusvar.h>
#include <luna68k/luna68k/isr.h>

#include <ioconf.h>

#include "psgpam_enc.h"
#include "xpcmd.h"
#include "xplx/xplxdefs.h"

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

struct psgpam_softc {
	device_t sc_dev;
	vaddr_t sc_shm_base;
	vsize_t sc_shm_size;

	void (*sc_intr)(void *);
	void *sc_arg;

	kmutex_t sc_intr_lock;
	kmutex_t sc_thread_lock;

	int      sc_isopen;

	int      sc_started;
	int      sc_outcount;
	int      sc_xp_state;	//
	uint16_t sc_xp_addr;	// XP buffer addr

	int      sc_xp_enc;
	u_int    sc_sample_rate;
	int      sc_stride;
	int      sc_dynamic;

	int      sc_xp_rept;

	struct psgpam_codecvar sc_psgpam_codecvar;
};

static int  psgpam_match(device_t, cfdata_t, void *);
static void psgpam_attach(device_t, device_t, void *);

/* MI audio layer interface */
static int  psgpam_open(void *, int);
static void psgpam_close(void *);
#if defined(AUDIO2)
static int  psgpam_query_format(void *, audio_format_query_t *);
static int  psgpam_init_format(void *, int,
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
static int  psgpam_round_blocksize(void *, int, int,
  const audio_params_t *);
static size_t psgpam_round_buffersize(void *, int, size_t);

static int  psgpam_intr(void *);

static int  psgpam_sysctl_enc(SYSCTLFN_PROTO);
static int  psgpam_sysctl_dynamic(SYSCTLFN_PROTO);

CFATTACH_DECL_NEW(psgpam, sizeof(struct psgpam_softc),
	psgpam_match, psgpam_attach, NULL, NULL);

static int psgpam_matched;

static const struct audio_hw_if psgpam_hw_if = {
	.open			= psgpam_open,
	.close			= psgpam_close,
#if defined(AUDIO2)
	.query_format		= psgpam_query_format,
	.init_format		= psgpam_init_format,
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
	.round_blocksize        = psgpam_round_blocksize,
	.round_buffersize	= psgpam_round_buffersize,
};

static struct audio_device psgpam_device = {
	"PSG PAM",
	"0.2",
	"",
};

// fill by query_format
static struct audio_format psgpam_format = {
	NULL, AUMODE_PLAY, AUDIO_ENCODING_SLINEAR_BE, 16, 16,
	1, AUFMT_MONAURAL,
	1, {0},
};

/* private functions */
static
void
psgpam_xp_start(struct psgpam_softc *sc)
{
	uint16_t cycle_clk;
	uint8_t rept_clk;

	DPRINTF(3, "XP PAM starting..");
	if (xp_readmem8(PAM_RUN) != 0) {
		DPRINTF(1, "XP PAM already started???");
	}

	xp_writemem8(PAM_ENC, sc->sc_xp_enc);
	xp_cmd(DEVID_PAM, PAM_CMD_QUERY);
	cycle_clk = xp_readmem16le(PAM_CYCLE_CLK);
	rept_clk = xp_readmem8(PAM_REPT_CLK);
	sc->sc_xp_rept = (XP_CPU_FREQ / sc->sc_sample_rate
		- cycle_clk) / rept_clk;
	xp_writemem8(PAM_REPT, sc->sc_xp_rept);
	DPRINTF(3, "ENC=%d REPT=%d\n", sc->sc_xp_enc, sc->sc_xp_rept);

	xp_intr5_enable();
	xp_cmd_nowait(DEVID_PAM, PAM_CMD_START);

	DPRINTF(3, "XP PAM started\n");
}

/* MI MD API */

static int
psgpam_match(device_t parent, cfdata_t cf, void *aux)
{
	struct xpbus_attach_args *xa = aux;

	if (psgpam_matched)
		return 0;

	if (strcmp(xa->xa_name, psgpam_cd.cd_name))
		return 0;

	psgpam_matched = 1;
	return 1;
}

static void
psgpam_attach(device_t parent, device_t self, void *aux)
{
	struct psgpam_softc *sc;
	const struct sysctlnode *node;

#if defined(AUDIO_DEBUG_MLOG)
	audio_mlog_init();
#endif

	sc = device_private(self);
	sc->sc_dev = self;

	aprint_normal(": HD647180X I/O processor as PSG PAM\n");

	sc->sc_shm_base = XP_SHM_BASE;
	sc->sc_shm_size = XP_SHM_SIZE;

	sc->sc_xp_enc = PAM_ENC_PAM2A;
	sc->sc_sample_rate = 8000;
	sc->sc_stride = 2;
	sc->sc_dynamic = 1;

	mutex_init(&sc->sc_thread_lock, MUTEX_DEFAULT, IPL_NONE);
	mutex_init(&sc->sc_intr_lock, MUTEX_DEFAULT, IPL_SCHED);

	isrlink_autovec(psgpam_intr, sc, 5, ISRPRI_TTYNOBUF);

	sysctl_createv(NULL, 0, NULL, &node,
		0,
		CTLTYPE_NODE, device_xname(sc->sc_dev),
		SYSCTL_DESCR("psgpam"),
		NULL, 0,
		NULL, 0,
		CTL_HW,
		CTL_CREATE, CTL_EOL);
	if (node != NULL) {
		sysctl_createv(NULL, 0, NULL, NULL,
			CTLFLAG_READWRITE,
			CTLTYPE_INT, "enc",
			SYSCTL_DESCR("PSGPAM encoding"),
			psgpam_sysctl_enc, 0, (void *)sc, 0,
			CTL_HW, node->sysctl_num,
			CTL_CREATE, CTL_EOL);
		sysctl_createv(NULL, 0, NULL, NULL,
			CTLFLAG_READWRITE,
			CTLTYPE_INT, "dynamic",
			SYSCTL_DESCR("PSGPAM dynamic offset"),
			psgpam_sysctl_dynamic, 0, (void *)sc, 0,
			CTL_HW, node->sysctl_num,
			CTL_CREATE, CTL_EOL);
	}


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

	u_int a;
	a = xp_acquire(DEVID_PAM, 0);
	if (a == 0)
		return EBUSY;

	// firmware transfer
	xp_ensure_firmware();

	sc->sc_xp_state = 0;
	sc->sc_started = 0;
	sc->sc_outcount = 0;
	sc->sc_isopen = 1;

	return 0;
}

static void
psgpam_close(void *hdl)
{
	struct psgpam_softc *sc;

	sc = hdl;

	xp_intr5_disable();

	xp_release(DEVID_PAM);

	sc->sc_isopen = 0;

	DPRINTF(1, "%s\n", __func__);
}

#if defined(AUDIO2)
static int
psgpam_query_format(void *hdl, audio_format_query_t *afp)
{
	struct psgpam_softc *sc;
	u_int a;
	u_int freq;
	uint16_t cycle_clk;
	uint8_t rept_clk;
	uint8_t rept_max;
	int clk;
	int i;

	sc = hdl;

	if (!sc->sc_isopen) {
		a = xp_acquire(DEVID_PAM, 0);
		if (a == 0)
			return EINVAL;
		xp_ensure_firmware();
	}

	xp_writemem8(PAM_ENC, sc->sc_xp_enc);
	xp_cmd(DEVID_PAM, PAM_CMD_QUERY);

	cycle_clk = xp_readmem16le(PAM_CYCLE_CLK);
	rept_clk = xp_readmem8(PAM_REPT_CLK);
	rept_max = xp_readmem8(PAM_REPT_MAX);
	if (rept_max >= AUFMT_MAX_FREQUENCIES) {
		rept_max = AUFMT_MAX_FREQUENCIES - 1;
	}

	for (i = 0; i <= rept_max; i++) {
		clk = cycle_clk + i * rept_clk;
		freq = XP_CPU_FREQ / clk;
		psgpam_format.frequency[i] = freq;
	}
	psgpam_format.frequency_type = rept_max + 1;

	if (!sc->sc_isopen) {
		xp_release(DEVID_PAM);
	}

	return audio_query_format(&psgpam_format, 1, afp);
}
#else
static int
psgpam_query_encoding(void *hdl, struct audio_encoding *ae)
{

	DPRINTF(1, "%s\n", __func__);

	// unsupported
	return EINVAL;
}
#endif

#if defined(AUDIO2)
static int
psgpam_init_format(void *hdl, int setmode,
	const audio_params_t *play, const audio_params_t *rec,
	audio_filter_reg_t *pfil, audio_filter_reg_t *rfil)
{
	// set_format は open 前に呼ばれる。
	// init_format の意味。

	struct psgpam_softc *sc;

	sc = hdl;
	DPRINTF(1, "%s: mode=%d %s/%dbit/%dch/%dHz\n", __func__,
	    setmode, audio_encoding_name(play->encoding),
	    play->precision, play->channels, play->sample_rate);

	sc->sc_sample_rate = play->sample_rate;

	// set filter
	pfil->param.sample_rate = sc->sc_sample_rate;
	pfil->param.encoding = AUDIO_ENCODING_NONE;
	pfil->param.channels = 1;

	switch (sc->sc_xp_enc) {
	 case PAM_ENC_PAM2A:
		if (sc->sc_dynamic) {
			pfil->codec = psgpam_aint_to_pam2a_d;
		} else {
			pfil->codec = psgpam_aint_to_pam2a;
		}
		pfil->param.precision = pfil->param.validbits = 16;
		break;
	 case PAM_ENC_PAM2B:
		if (sc->sc_dynamic) {
			pfil->codec = psgpam_aint_to_pam2b_d;
		} else {
			pfil->codec = psgpam_aint_to_pam2b;
		}
		pfil->param.precision = pfil->param.validbits = 16;
		break;
	 case PAM_ENC_PAM3A:
		if (sc->sc_dynamic) {
			pfil->codec = psgpam_aint_to_pam3a_d;
		} else {
			pfil->codec = psgpam_aint_to_pam3a;
		}
		pfil->param.precision = pfil->param.validbits = 32;
		break;
	 case PAM_ENC_PAM3B:
		if (sc->sc_dynamic) {
			pfil->codec = psgpam_aint_to_pam3b_d;
		} else {
			pfil->codec = psgpam_aint_to_pam3b;
		}
		pfil->param.precision = pfil->param.validbits = 32;
		break;
	}
	psgpam_init_context(&sc->sc_psgpam_codecvar, sc->sc_sample_rate);
	pfil->context = &sc->sc_psgpam_codecvar;

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

	markoffset = blksize - sc->sc_stride;

	if (sc->sc_xp_state == 0) {
		marker = XP_ATN_STAT;
		sc->sc_xp_addr = PAM_BUF;

		if (sc->sc_started == 0) {
			// if first transfer, interrupt at top of buffer.
			markoffset = 0;
		}
	} else {
		marker = XP_ATN_RELOAD;
	}

	// marking
	if (sc->sc_stride == 2) {
		uint16_t *markptr = (uint16_t*)((uint8_t*)block + markoffset);
		*markptr |= marker;
	} else {
		// stride == 4
		uint32_t *markptr = (uint32_t*)((uint8_t*)block + markoffset);
		*markptr |= marker;
	}

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
	uint32_t marker;

	DPRINTF(2, "%s\n", __func__);

	xp_intr5_disable();

	marker = XP_ATN_RESET;
	if (sc->sc_stride == 2) {
		uint16_t *markptr = xp_shmptr(PAM_BUF);
		*markptr |= marker;
	} else {
		// stride == 4
		uint32_t *markptr = xp_shmptr(PAM_BUF);
		*markptr |= marker;
	}

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
psgpam_round_blocksize(void *hdl, int bs, int mode,
  const audio_params_t *param)
{
#if 0
	if (bs < 16384) {
		return (16384 / bs) * bs;
	} else {
		return 16384;
	}
#else
	return bs;
#endif
}

static size_t
psgpam_round_buffersize(void *hdl, int direction, size_t bufsize)
{
	return bufsize;
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

/* sysctl */
static int
psgpam_sysctl_enc(SYSCTLFN_ARGS)
{
	struct sysctlnode node;
	struct psgpam_softc *sc;
	int t, error;

	node = *rnode;
	sc = node.sysctl_data;

	t = sc->sc_xp_enc;
	node.sysctl_data = &t;

	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL) {
		return error;
	}

	if (t < PAM_ENC_PAM2A)
		return EINVAL;
	if (t > PAM_ENC_PAM3B)
		return EINVAL;
	sc->sc_xp_enc = t;
	return 0;
}

static int
psgpam_sysctl_dynamic(SYSCTLFN_ARGS)
{
	struct sysctlnode node;
	struct psgpam_softc *sc;
	int t, error;

	node = *rnode;
	sc = node.sysctl_data;

	t = sc->sc_dynamic;
	node.sysctl_data = &t;

	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL) {
		return error;
	}

	sc->sc_dynamic = t;
	return 0;
}
