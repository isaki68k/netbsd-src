/* vi:set ts=8: */
/*
 * Locking: there are two locks.
 *
 * - sc_lock, provided by the underlying driver.  This is an adaptive lock,
 *   returned in the second parameter to hw_if->get_locks().  It is known
 *   as the "thread lock".
 *
 *   It serializes access to state in all places except the
 *   driver's interrupt service routine.  This lock is taken from process
 *   context (example: access to /dev/audio).  It is also taken from soft
 *   interrupt handlers in this module, primarily to serialize delivery of
 *   wakeups.  This lock may be used/provided by modules external to the
 *   audio subsystem, so take care not to introduce a lock order problem.
 *   LONG TERM SLEEPS MUST NOT OCCUR WITH THIS LOCK HELD.
 *
 * - sc_intr_lock, provided by the underlying driver.  This may be either a
 *   spinlock (at IPL_SCHED or IPL_VM) or an adaptive lock (IPL_NONE or
 *   IPL_SOFT*), returned in the first parameter to hw_if->get_locks().  It
 *   is known as the "interrupt lock".
 *
 *   It provides atomic access to the device's hardware state, and to audio
 *   channel data that may be accessed by the hardware driver's ISR.
 *   In all places outside the ISR, sc_lock must be held before taking
 *   sc_intr_lock.  This is to ensure that groups of hardware operations are
 *   made atomically.  SLEEPS CANNOT OCCUR WITH THIS LOCK HELD.
 *
 * List of hardware interface methods, and which locks are held when each
 * is called by this module:
 *
 *	METHOD			INTR	THREAD  NOTES
 *	----------------------- ------- -------	-------------------------
 *	open 			x	x
 *	close 			x	x
 *	drain 			x	x
 *	query_encoding		-	x
 *	query_format		-	x	たぶん
 *	set_params 		-	x
 *	round_blocksize		-	x
 *	commit_settings		-	x
 *	init_output 		x	x
 *	init_input 		x	x
 *	start_output 		x	x
 *	start_input 		x	x
 *	halt_output 		x	x
 *	halt_input 		x	x
 *	speaker_ctl 		x	x
 *	getdev 			-	x
 *	setfd 			-	x
 *	set_port 		-	x
 *	get_port 		-	x
 *	query_devinfo 		-	x
 *	allocm 			-	-	Called at attach time
 *	freem 			-	-	Called at attach time
 *	round_buffersize 	-	x
 *	mappage 		-	-	Mem. unchanged after attach
 *	get_props 		-	x
 *	trigger_output 		x	x
 *	trigger_input 		x	x
 *	dev_ioctl 		-	x
 *	get_locks 		-	-	Called at attach time
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#define OLD_FILTER

#ifdef _KERNEL_OPT
#include "audio.h"
#include "midi.h"
#endif

#if NAUDIO > 0

#include <sys/types.h>
#include <sys/param.h>
#include <sys/audioio.h>
#include <sys/conf.h>
#include <sys/cpu.h>
#include <sys/device.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/intr.h>
#include <sys/ioctl.h>
#include <sys/kauth.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/select.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/syslog.h>
#include <sys/vnode.h>

#include <dev/audio_if.h>
// -- audiovar.h ここから
#include <dev/audio/audiovar.h>
#include <dev/audio/aumix.h>

/* Interfaces for audiobell. */
int audiobellopen(dev_t, int, int, struct lwp *, struct file **);
int audiobellclose(struct file *);
int audiobellwrite(struct file *, off_t *, struct uio *, kauth_cred_t, int);
int audiobellioctl(struct file *, u_long, void *);

#define AUDIO_N_PORTS 4

struct au_mixer_ports {
	int	index;		/* index of port-selector mixerctl */
	int	master;		/* index of master mixerctl */
	int	nports;		/* number of selectable ports */
	bool	isenum;		/* selector is enum type */
	u_int	allports;	/* all aumasks or'd */
	u_int	aumask[AUDIO_N_PORTS];	/* exposed value of "ports" */
	int	misel [AUDIO_N_PORTS];	/* ord of port, for selector */
	int	miport[AUDIO_N_PORTS];	/* index of port's mixerctl */
	bool	isdual;		/* has working mixerout */
	int	mixerout;	/* ord of mixerout, for dual case */
	int	cur_port;	/* the port that gain actually controls when
				   mixerout is selected, for dual case */
};

struct audio_softc {
	device_t	dev;

	// NULL なら sc はあるが autoconfig に失敗したので無効になってる
	// audio1 at ... attached
	// audio1: ... config failed
	// みたいな状態
	const struct audio_hw_if *hw_if; /* Hardware interface */
	void		*hw_hdl;	/* Hardware driver handle */
	device_t	sc_dev;		/* Hardware device struct */

	SLIST_HEAD(, audio_file) sc_files;	/* list of open descriptor */

	audio_trackmixer_t *sc_pmixer;	/* null if play not supported by hw */
	audio_trackmixer_t *sc_rmixer;	/* null if rec not supported by hw */

	audio_format2_t sc_phwfmt;
	audio_format2_t sc_rhwfmt;

	bool		sc_full_duplex;		/* device in full duplex mode */
	bool		sc_can_playback;	/* device can playback */
	bool		sc_can_capture;		/* device can capture */

	int sc_popens;
	int sc_ropens;
	bool			sc_pbusy;	/* output DMA in progress */
	bool			sc_rbusy;	/* input DMA in progress */

	int		sc_eof;		/* EOF, i.e. zero sized write, counter */

	struct audio_info sc_ai;	/* recent info for /dev/sound */

	struct	selinfo sc_wsel; /* write selector */
	struct	selinfo sc_rsel; /* read selector */
	pid_t		sc_async_audio;	/* process who wants audio SIGIO */
	void		*sc_sih_rd;
	void		*sc_sih_wr;
	struct	mixer_asyncs {
		struct mixer_asyncs *next;
		pid_t	pid;
	} *sc_async_mixer;  /* processes who want mixer SIGIO */

	/* Locks and sleep channels for reading, writing and draining. */
	kmutex_t	*sc_intr_lock;
	kmutex_t	*sc_lock;
	kcondvar_t	sc_rchan;
	kcondvar_t	sc_wchan;
	bool		sc_dying;

	mixer_ctrl_t	*sc_mixer_state;
	int		sc_nmixer_states;
	int		sc_static_nmixer_states;
	struct au_mixer_ports sc_inports;
	struct au_mixer_ports sc_outports;
	u_int	sc_lastgain;
	int		sc_monitor_port;

	struct audio_encoding *sc_encodings;
	int sc_encodings_count;
};

// -- audiovar.h ここまで
#include <dev/auconv.h>
#include <dev/auvolconv.h>

#include <machine/endian.h>

#include <uvm/uvm.h>

#define AUDIO_DEBUG	1
#ifdef AUDIO_DEBUG
#define DPRINTF(x)	if (audiodebug) printf x
#define DPRINTFN(n,x)	if (audiodebug>(n)) printf x
int	audiodebug = AUDIO_DEBUG;
#else
#define DPRINTF(x)
#define DPRINTFN(n,x)
#endif

#define ROUNDSIZE(x)	(x) &= -16	/* round to nice boundary */
#define SPECIFIED(x)	((x) != ~0)
#define SPECIFIED_CH(x)	((x) != (u_char)~0)

/* #define AUDIO_PM_IDLE */
#ifdef AUDIO_PM_IDLE
int	audio_idle_timeout = 30;
#endif

#if BYTE_ORDER == LITTLE_ENDIAN
#define AUDIO_ENCODING_SLINEAR_NE AUDIO_ENCODING_SLINEAR_LE
#define AUDIO_ENCODING_ULINEAR_NE AUDIO_ENCODING_ULINEAR_LE
#else
#define AUDIO_ENCODING_SLINEAR_NE AUDIO_ENCODING_SLINEAR_BE
#define AUDIO_ENCODING_ULINEAR_NE AUDIO_ENCODING_ULINEAR_BE
#endif

struct portname {
	const char *name;
	int mask;
};
typedef struct uio_fetcher {
	stream_fetcher_t base;
	struct uio *uio;
	int usedhigh;
	int last_used;
} uio_fetcher_t;

// ここに関数プロトタイプ
// できれば static つけて統一したい

static int	audiomatch(device_t, cfdata_t, void *);
static void	audioattach(device_t, device_t, void *);
static int	audiodetach(device_t, int);
static int	audioactivate(device_t, enum devact);
// spkr* at audio? のため?
//static void	audiochilddet(device_t, device_t);
//static int	audiorescan(device_t, const char *, const int *);

static int	audio_modcmd(modcmd_t, void *);

#ifdef AUDIO_PM_IDLE
static void	audio_idle(void *);
static void	audio_activity(device_t, devactive_t);
#endif

static bool	audio_suspend(device_t dv, const pmf_qual_t *);
static bool	audio_resume(device_t dv, const pmf_qual_t *);
static void	audio_volume_down(device_t);
static void	audio_volume_up(device_t);
static void	audio_volume_toggle(device_t);

static void	audio_mixer_capture(struct audio_softc *);
static void	audio_mixer_restore(struct audio_softc *);


static void	audio_softintr_rd(void *);
static void	audio_softintr_wr(void *);

static int audio_enter(dev_t, struct audio_softc **);
static void audio_exit(struct audio_softc *);
static int audio_waitio(struct audio_softc *, kcondvar_t *, audio_track_t *);

static int audioclose(struct file *);
static int audioread(struct file *, off_t *, struct uio *, kauth_cred_t, int);
static int audiowrite(struct file *, off_t *, struct uio *, kauth_cred_t, int);
static int audioioctl(struct file *, u_long, void *);
static int audiopoll(struct file *, int);
static int audiokqfilter(struct file *, struct knote *);
static int audiommap(struct file *, off_t *, size_t, int, int *, int *,
			  struct uvm_object **, int *);
static int audiostat(struct file *, struct stat *);

static int audio_open(dev_t, struct audio_softc *, int, int, struct lwp *,
		      struct file **);
static int audio_drain(struct audio_softc *, audio_track_t *);
static int audio_close(struct audio_softc *, int, audio_file_t *);
static int audio_read(struct audio_softc *, struct uio *, int, audio_file_t *);
//static int audio_write(struct audio_softc *, struct uio *, int, audio_file_t *);
static void audio_clear(struct audio_softc *);
static void audio_clear_intr_unlocked(struct audio_softc *sc);
static int audio_ioctl(dev_t, struct audio_softc *, u_long, void *, int,
		       struct lwp *, audio_file_t *);
static int audio_poll(struct audio_softc *, int, struct lwp *, audio_file_t *);
static int audio_kqfilter(struct audio_softc *, audio_file_t *, struct knote *);
static int audio_mmap(struct audio_softc *, off_t *, size_t, int, int *, int *,
	struct uvm_object **, int *, audio_file_t *);

static int audiostartr(struct audio_softc *);
static int audiostartp(struct audio_softc *);
static void audio_pintr(void *);
static void audio_rintr(void *);

static int audio_query_devinfo(struct audio_softc *, mixer_devinfo_t *);

static int audio_file_set_defaults(struct audio_softc *, audio_file_t *);
static int audio_file_setinfo(struct audio_softc *, audio_file_t *,
	const struct audio_info *);
static int audio_file_setinfo_check(audio_format2_t *,
	const struct audio_prinfo *);
static int audio_file_setinfo_set(audio_track_t *, audio_format2_t *,
	const struct audio_prinfo *, bool);
static int audio_setinfo_hw(struct audio_softc *, struct audio_info *);
static int audio_set_params(struct audio_softc *);
static int audiogetinfo(struct audio_softc *, struct audio_info *, int,
	audio_file_t *);
static int audio_getenc(struct audio_softc *, struct audio_encoding *);
static int audio_get_props(struct audio_softc *);
static bool audio_can_playback(struct audio_softc *);
static bool audio_can_capture(struct audio_softc *);
static int audio_check_params(struct audio_params *);
static int audio_check_params2(const audio_format2_t *);
static int xxx_select_freq(const struct audio_format *);
static int xxx_config_hwfmt(struct audio_softc *, audio_format2_t *, int);
static int audio_xxx_config(struct audio_softc *, int);
static void audio_prepare_enc_xxx(struct audio_softc *sc);

#ifdef OLD_FILTER
static void stream_filter_list_append(stream_filter_list_t *,
		stream_filter_factory_t, const audio_params_t *);
static void stream_filter_list_prepend(stream_filter_list_t *,
		stream_filter_factory_t, const audio_params_t *);
static void stream_filter_list_set(stream_filter_list_t *, int,
		stream_filter_factory_t, const audio_params_t *);
#endif

static inline struct audio_params
format2_to_params(const audio_format2_t *f2)
{
	audio_params_t p;

	p.sample_rate = f2->sample_rate;
	p.channels    = f2->channels;
	p.encoding    = f2->encoding;
	p.validbits   = f2->precision;
	p.precision   = f2->stride;
	return p;
}

static inline audio_format2_t
params_to_format2(const struct audio_params *p)
{
	audio_format2_t f2;

	f2.sample_rate = p->sample_rate;
	f2.channels    = p->channels;
	f2.encoding    = p->encoding;
	f2.precision   = p->validbits;
	f2.stride      = p->precision;
	return f2;
}


static void mixer_init(struct audio_softc *);
static int mixer_open(dev_t, struct audio_softc *, int, int, struct lwp *,
		      struct file **);
static int mixer_close(struct audio_softc *, int, audio_file_t *);
static int mixer_ioctl(struct audio_softc *, u_long, void *, int, struct lwp *);
static void mixer_remove(struct audio_softc *);
static void mixer_signal(struct audio_softc *);

static int au_portof(struct audio_softc *sc, char *, int);

static void au_setup_ports(struct audio_softc *, struct au_mixer_ports *,
			   mixer_devinfo_t *, const struct portname *);
static int au_set_lr_value(struct audio_softc *, mixer_ctrl_t *, int, int);
static int au_get_lr_value(struct audio_softc *, mixer_ctrl_t *, int *, int *);
static int au_set_gain(struct audio_softc *, struct au_mixer_ports *,
		       int, int);
static void au_get_gain(struct audio_softc *, struct au_mixer_ports *,
			u_int *, u_char *);
static int au_set_port(struct audio_softc *, struct au_mixer_ports *,
		       u_int);
static int au_get_port(struct audio_softc *, struct au_mixer_ports *);
static int au_set_monitor_gain(struct audio_softc *sc, int);
static int au_get_monitor_gain(struct audio_softc *sc);
static int audio_get_port(struct audio_softc *, mixer_ctrl_t *);
static int audio_set_port(struct audio_softc *, mixer_ctrl_t *);

static dev_type_open(audioopen);
/* XXXMRG use more dev_type_xxx */

const struct cdevsw audio_cdevsw = {
	.d_open = audioopen,
	.d_close = noclose,
	.d_read = noread,
	.d_write = nowrite,
	.d_ioctl = noioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = nopoll,
	.d_mmap = nommap,
	.d_kqfilter = nokqfilter,
	.d_discard = nodiscard,
	.d_flag = D_OTHER | D_MPSAFE
};

const struct fileops audio_fileops = {
	.fo_read = audioread,
	.fo_write = audiowrite,
	.fo_ioctl = audioioctl,
	.fo_fcntl = fnullop_fcntl,
	.fo_stat = audiostat,
	.fo_poll = audiopoll,
	.fo_close = audioclose,
	.fo_mmap = audiommap,
	.fo_kqfilter = audiokqfilter,
	.fo_restart = fnullop_restart
};

/* The default audio mode: 8 kHz mono mu-law */
const struct audio_params audio_default = {
	.sample_rate = 8000,
	.encoding = AUDIO_ENCODING_ULAW,
	.precision = 8,
	.validbits = 8,
	.channels = 1,
};

static const struct portname itable[] = {
	{ AudioNmicrophone,	AUDIO_MICROPHONE },
	{ AudioNline,		AUDIO_LINE_IN },
	{ AudioNcd,		AUDIO_CD },
	{ 0, 0 }
};
static const struct portname otable[] = {
	{ AudioNspeaker,	AUDIO_SPEAKER },
	{ AudioNheadphone,	AUDIO_HEADPHONE },
	{ AudioNline,		AUDIO_LINE_OUT },
	{ 0, 0 }
};

CFATTACH_DECL3_NEW(audio, sizeof(struct audio_softc),
    audiomatch, audioattach, audiodetach, audioactivate, NULL, NULL,
    DVF_DETACH_SHUTDOWN);

extern struct cfdriver audio_cd;

static int
audiomatch(device_t parent, cfdata_t match, void *aux)
{
	struct audio_attach_args *sa;

	sa = aux;
	DPRINTF(("%s: type=%d sa=%p hw=%p\n",
	     __func__, sa->type, sa, sa->hwif));
	return (sa->type == AUDIODEV_TYPE_AUDIO) ? 1 : 0;
}

static void
audioattach(device_t parent, device_t self, void *aux)
{
	struct audio_softc *sc;
	struct audio_attach_args *sa;
	const struct audio_hw_if *hw_if;
	void *hdlp;
	bool is_indep;
	int props;
	int error;

	sc = device_private(self);
	sc->dev = self;
	sa = (struct audio_attach_args *)aux;
	hw_if = sa->hwif;
	hdlp = sa->hdl;

	cv_init(&sc->sc_rchan, "audiord");
	cv_init(&sc->sc_wchan, "audiowr");

	if (hw_if == NULL || hw_if->get_locks == NULL) {
		panic("audioattach: missing hw_if method");
	}

	hw_if->get_locks(hdlp, &sc->sc_intr_lock, &sc->sc_lock);

#ifdef DIAGNOSTIC
	if (hw_if->query_encoding == NULL ||
	    hw_if->set_params == NULL ||
	    (hw_if->start_output == NULL && hw_if->trigger_output == NULL) ||
	    (hw_if->start_input == NULL && hw_if->trigger_input == NULL) ||
	    hw_if->halt_output == NULL ||
	    hw_if->halt_input == NULL ||
	    hw_if->getdev == NULL ||
	    hw_if->set_port == NULL ||
	    hw_if->get_port == NULL ||
	    hw_if->query_devinfo == NULL ||
	    hw_if->get_props == NULL) {
		aprint_error(": missing method\n");
		return;
	}
#endif

	sc->hw_if = hw_if;
	sc->hw_hdl = hdlp;
	sc->sc_dev = parent;

	SLIST_INIT(&sc->sc_files);

	mutex_enter(sc->sc_lock);
	props = audio_get_props(sc);
	mutex_exit(sc->sc_lock);

	sc->sc_full_duplex = (props & AUDIO_PROP_FULLDUPLEX);
	if (sc->sc_full_duplex) {
		aprint_normal(": full duplex");
	} else {
		aprint_normal(": half duplex");
	}

	sc->sc_can_playback = (props & AUDIO_PROP_PLAYBACK);
	sc->sc_can_capture = (props & AUDIO_PROP_CAPTURE);
	is_indep = (props & AUDIO_PROP_INDEPENDENT);
	if (sc->sc_can_playback)
		aprint_normal(", playback");
	if (sc->sc_can_capture)
		aprint_normal(", capture");
	if ((props & AUDIO_PROP_MMAP) != 0)
		aprint_normal(", mmap");
	if (is_indep)
		aprint_normal(", independent");

	aprint_naive("\n");
	aprint_normal("\n");

	// play と capture が両方立ってない状況はたぶん起きない

	/* probe hw params */
	error = audio_xxx_config(sc, is_indep);

	if (sc->sc_can_playback == false && sc->sc_can_capture == false) {
		sc->hw_if = NULL;
		return;
	}

	/* init track mixer */
	if (sc->sc_can_playback) {
		sc->sc_pmixer = kmem_zalloc(sizeof(*sc->sc_pmixer),
		    KM_SLEEP);
		audio_mixer_init(sc, sc->sc_pmixer, AUMODE_PLAY);
		aprint_normal_dev(sc->dev,
		    "slinear%d, %dch, %dHz for playback\n",
		    AUDIO_INTERNAL_BITS,
		    sc->sc_pmixer->hwbuf.fmt.channels,
		    sc->sc_pmixer->hwbuf.fmt.sample_rate);
	}
	if (sc->sc_can_capture) {
		sc->sc_rmixer = kmem_zalloc(sizeof(*sc->sc_rmixer),
		    KM_SLEEP);
		audio_mixer_init(sc, sc->sc_rmixer, AUMODE_RECORD);
		aprint_normal_dev(sc->dev,
		    "slinear%d, %dch, %dHz for recording\n",
		    AUDIO_INTERNAL_BITS,
		    sc->sc_rmixer->hwbuf.fmt.channels,
		    sc->sc_rmixer->hwbuf.fmt.sample_rate);
	}

	/* Set hw full-duplex if necessary */
	if (sc->sc_full_duplex) {
		if (sc->hw_if->setfd) {
			mutex_enter(sc->sc_lock);
			error = sc->hw_if->setfd(sc->hw_hdl, 1);
			mutex_exit(sc->sc_lock);
			if (error) {
				aprint_error_dev(sc->dev,
				    "Setting full-duplex failed: %d\n",
				    error);
				sc->hw_if = NULL;
				return;
			}
		}
	}

	sc->sc_sih_rd = softint_establish(SOFTINT_SERIAL | SOFTINT_MPSAFE,
	    audio_softintr_rd, sc);
	sc->sc_sih_wr = softint_establish(SOFTINT_SERIAL | SOFTINT_MPSAFE,
	    audio_softintr_wr, sc);

	mixer_init(sc);
	DPRINTF(("audio_attach: inputs ports=0x%x, input master=%d, "
		 "output ports=0x%x, output master=%d\n",
		 sc->sc_inports.allports, sc->sc_inports.master,
		 sc->sc_outports.allports, sc->sc_outports.master));

	// sysctl ここ?

	selinit(&sc->sc_rsel);
	selinit(&sc->sc_wsel);

#ifdef AUDIO_PM_IDLE
	callout_init(&sc->sc_idle_counter, 0);
	callout_setfunc(&sc->sc_idle_counter, audio_idle, self);
#endif

	if (!pmf_device_register(self, audio_suspend, audio_resume))
		aprint_error_dev(self, "couldn't establish power handler\n");
#ifdef AUDIO_PM_IDLE
	if (!device_active_register(self, audio_activity))
		aprint_error_dev(self, "couldn't register activity handler\n");
#endif

	if (!pmf_event_register(self, PMFE_AUDIO_VOLUME_DOWN,
	    audio_volume_down, true))
		aprint_error_dev(self, "couldn't add volume down handler\n");
	if (!pmf_event_register(self, PMFE_AUDIO_VOLUME_UP,
	    audio_volume_up, true))
		aprint_error_dev(self, "couldn't add volume up handler\n");
	if (!pmf_event_register(self, PMFE_AUDIO_VOLUME_TOGGLE,
	    audio_volume_toggle, true))
		aprint_error_dev(self, "couldn't add volume toggle handler\n");

#ifdef AUDIO_PM_IDLE
	callout_schedule(&sc->sc_idle_counter, audio_idle_timeout * hz);
#endif

	// audiorescan いらないはず
}

/* called from audioattach() */
static void
mixer_init(struct audio_softc *sc)
{
	mixer_devinfo_t mi;
	int iclass, mclass, oclass, rclass;
	int record_master_found, record_source_found;

	iclass = mclass = oclass = rclass = -1;
	sc->sc_inports.index = -1;
	sc->sc_inports.master = -1;
	sc->sc_inports.nports = 0;
	sc->sc_inports.isenum = false;
	sc->sc_inports.allports = 0;
	sc->sc_inports.isdual = false;
	sc->sc_inports.mixerout = -1;
	sc->sc_inports.cur_port = -1;
	sc->sc_outports.index = -1;
	sc->sc_outports.master = -1;
	sc->sc_outports.nports = 0;
	sc->sc_outports.isenum = false;
	sc->sc_outports.allports = 0;
	sc->sc_outports.isdual = false;
	sc->sc_outports.mixerout = -1;
	sc->sc_outports.cur_port = -1;
	sc->sc_monitor_port = -1;
	/*
	 * Read through the underlying driver's list, picking out the class
	 * names from the mixer descriptions. We'll need them to decode the
	 * mixer descriptions on the next pass through the loop.
	 */
	mutex_enter(sc->sc_lock);
	for(mi.index = 0; ; mi.index++) {
		if (audio_query_devinfo(sc, &mi) != 0)
			break;
		 /*
		  * The type of AUDIO_MIXER_CLASS merely introduces a class.
		  * All the other types describe an actual mixer.
		  */
		if (mi.type == AUDIO_MIXER_CLASS) {
			if (strcmp(mi.label.name, AudioCinputs) == 0)
				iclass = mi.mixer_class;
			if (strcmp(mi.label.name, AudioCmonitor) == 0)
				mclass = mi.mixer_class;
			if (strcmp(mi.label.name, AudioCoutputs) == 0)
				oclass = mi.mixer_class;
			if (strcmp(mi.label.name, AudioCrecord) == 0)
				rclass = mi.mixer_class;
		}
	}
	mutex_exit(sc->sc_lock);

	/* Allocate save area.  Ensure non-zero allocation. */
	sc->sc_static_nmixer_states = mi.index;
	sc->sc_static_nmixer_states++;
	sc->sc_nmixer_states = sc->sc_static_nmixer_states;
	sc->sc_mixer_state = kmem_zalloc(sizeof(mixer_ctrl_t) *
	    (sc->sc_nmixer_states + 1), KM_SLEEP);

	/*
	 * This is where we assign each control in the "audio" model, to the
	 * underlying "mixer" control.  We walk through the whole list once,
	 * assigning likely candidates as we come across them.
	 */
	record_master_found = 0;
	record_source_found = 0;
	mutex_enter(sc->sc_lock);
	for(mi.index = 0; ; mi.index++) {
		if (audio_query_devinfo(sc, &mi) != 0)
			break;
		KASSERT(mi.index < sc->sc_nmixer_states);
		if (mi.type == AUDIO_MIXER_CLASS)
			continue;
		if (mi.mixer_class == iclass) {
			/*
			 * AudioCinputs is only a fallback, when we don't
			 * find what we're looking for in AudioCrecord, so
			 * check the flags before accepting one of these.
			 */
			if (strcmp(mi.label.name, AudioNmaster) == 0
			    && record_master_found == 0)
				sc->sc_inports.master = mi.index;
			if (strcmp(mi.label.name, AudioNsource) == 0
			    && record_source_found == 0) {
				if (mi.type == AUDIO_MIXER_ENUM) {
				    int i;
				    for(i = 0; i < mi.un.e.num_mem; i++)
					if (strcmp(mi.un.e.member[i].label.name,
						    AudioNmixerout) == 0)
						sc->sc_inports.mixerout =
						    mi.un.e.member[i].ord;
				}
				au_setup_ports(sc, &sc->sc_inports, &mi,
				    itable);
			}
			if (strcmp(mi.label.name, AudioNdac) == 0 &&
			    sc->sc_outports.master == -1)
				sc->sc_outports.master = mi.index;
		} else if (mi.mixer_class == mclass) {
			if (strcmp(mi.label.name, AudioNmonitor) == 0)
				sc->sc_monitor_port = mi.index;
		} else if (mi.mixer_class == oclass) {
			if (strcmp(mi.label.name, AudioNmaster) == 0)
				sc->sc_outports.master = mi.index;
			if (strcmp(mi.label.name, AudioNselect) == 0)
				au_setup_ports(sc, &sc->sc_outports, &mi,
				    otable);
		} else if (mi.mixer_class == rclass) {
			/*
			 * These are the preferred mixers for the audio record
			 * controls, so set the flags here, but don't check.
			 */
			if (strcmp(mi.label.name, AudioNmaster) == 0) {
				sc->sc_inports.master = mi.index;
				record_master_found = 1;
			}
#if 1	/* Deprecated. Use AudioNmaster. */
			if (strcmp(mi.label.name, AudioNrecord) == 0) {
				sc->sc_inports.master = mi.index;
				record_master_found = 1;
			}
			if (strcmp(mi.label.name, AudioNvolume) == 0) {
				sc->sc_inports.master = mi.index;
				record_master_found = 1;
			}
#endif
			if (strcmp(mi.label.name, AudioNsource) == 0) {
				if (mi.type == AUDIO_MIXER_ENUM) {
				    int i;
				    for(i = 0; i < mi.un.e.num_mem; i++)
					if (strcmp(mi.un.e.member[i].label.name,
						    AudioNmixerout) == 0)
						sc->sc_inports.mixerout =
						    mi.un.e.member[i].ord;
				}
				au_setup_ports(sc, &sc->sc_inports, &mi,
				    itable);
				record_source_found = 1;
			}
		}
	}
	mutex_exit(sc->sc_lock);
}

static int
audioactivate(device_t self, enum devact act)
{
	struct audio_softc *sc = device_private(self);

	switch (act) {
	case DVACT_DEACTIVATE:
		mutex_enter(sc->sc_lock);
		sc->sc_dying = true;
		mutex_exit(sc->sc_lock);
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

static int
audiodetach(device_t self, int flags)
{
	struct audio_softc *sc;
	int maj, mn;
	//int rc;

	sc = device_private(self);
	DPRINTF(("%s: sc=%p flags=%d\n", __func__, sc, flags));

	/* Start draining existing accessors of the device. */
	// なぜここで config_detach_children() ?
	mutex_enter(sc->sc_lock);
	sc->sc_dying = true;
	cv_broadcast(&sc->sc_wchan);
	cv_broadcast(&sc->sc_rchan);
	mutex_exit(sc->sc_lock);

	/* locate the major number */
	maj = cdevsw_lookup_major(&audio_cdevsw);

	/*
	 * Nuke the vnodes for any open instances (calls close).
	 * Will wait until any activity on the device nodes has ceased.
	 *
	 * XXXAD NOT YET.
	 *
	 * XXXAD NEED TO PREVENT NEW REFERENCES THROUGH AUDIO_ENTER().
	 */
	mn = device_unit(self);
	vdevgone(maj, mn | SOUND_DEVICE,    mn | SOUND_DEVICE, VCHR);
	vdevgone(maj, mn | AUDIO_DEVICE,    mn | AUDIO_DEVICE, VCHR);
	vdevgone(maj, mn | AUDIOCTL_DEVICE, mn | AUDIOCTL_DEVICE, VCHR);
	vdevgone(maj, mn | MIXER_DEVICE,    mn | MIXER_DEVICE, VCHR);

	pmf_event_deregister(self, PMFE_AUDIO_VOLUME_DOWN,
	    audio_volume_down, true);
	pmf_event_deregister(self, PMFE_AUDIO_VOLUME_UP,
	    audio_volume_up, true);
	pmf_event_deregister(self, PMFE_AUDIO_VOLUME_TOGGLE,
	    audio_volume_toggle, true);

#ifdef AUDIO_PM_IDLE
	callout_halt(&sc->sc_idle_counter, sc->sc_lock);

	device_active_deregister(self, audio_activity);
#endif

	pmf_device_deregister(self);

	/* free resources */
	// ここでリソース解放

	if (sc->sc_sih_rd) {
		softint_disestablish(sc->sc_sih_rd);
		sc->sc_sih_rd = NULL;
	}
	if (sc->sc_sih_wr) {
		softint_disestablish(sc->sc_sih_wr);
		sc->sc_sih_wr = NULL;
	}

#ifdef AUDIO_PM_IDLE
	callout_destroy(&sc->sc_idle_counter);
#endif
	seldestroy(&sc->sc_rsel);
	seldestroy(&sc->sc_wsel);

	cv_destroy(&sc->sc_rchan);
	cv_destroy(&sc->sc_wchan);

	return 0;
}

/*
 * Called from hardware driver.  This is where the MI audio driver gets
 * probed/attached to the hardware driver.
 */
device_t
audio_attach_mi(const struct audio_hw_if *ahwp, void *hdlp, device_t dev)
{
	struct audio_attach_args arg;

#ifdef DIAGNOSTIC
	if (ahwp == NULL) {
		aprint_error("audio_attach_mi: NULL\n");
		return 0;
	}
#endif
	arg.type = AUDIODEV_TYPE_AUDIO;
	arg.hwif = ahwp;
	arg.hdl = hdlp;
	return config_found(dev, &arg, audioprint);
}

#ifdef AUDIO_DEBUG
void	audio_printsc(struct audio_softc *);
void	audio_print_params(const char *, struct audio_params *);
void	audio_print_format2(const char *, const audio_format2_t *);

void
audio_printsc(struct audio_softc *sc)
{
	printf("%s not impl\n", __func__);
}

void
audio_print_params(const char *s, struct audio_params *p)
{
	printf("%s enc=%u %uch %u/%ubit %uHz\n", s, p->encoding, p->channels,
	       p->validbits, p->precision, p->sample_rate);
}

void
audio_print_format2(const char *s, const audio_format2_t *f2)
{
	struct audio_params p = format2_to_params(f2);
	audio_print_params(s, &p);
}
#endif

#ifdef OLD_FILTER
static void
stream_filter_list_append(stream_filter_list_t *list,
			  stream_filter_factory_t factory,
			  const audio_params_t *param)
{

	if (list->req_size >= AUDIO_MAX_FILTERS) {
		printf("%s: increase AUDIO_MAX_FILTERS in sys/dev/audio_if.h\n",
		       __func__);
		return;
	}
	list->filters[list->req_size].factory = factory;
	list->filters[list->req_size].param = *param;
	list->req_size++;
}

static void
stream_filter_list_set(stream_filter_list_t *list, int i,
		       stream_filter_factory_t factory,
		       const audio_params_t *param)
{

	if (i < 0 || i >= AUDIO_MAX_FILTERS) {
		printf("%s: invalid index: %d\n", __func__, i);
		return;
	}

	list->filters[i].factory = factory;
	list->filters[i].param = *param;
	if (list->req_size <= i)
		list->req_size = i + 1;
}

static void
stream_filter_list_prepend(stream_filter_list_t *list,
			   stream_filter_factory_t factory,
			   const audio_params_t *param)
{

	if (list->req_size >= AUDIO_MAX_FILTERS) {
		printf("%s: increase AUDIO_MAX_FILTERS in sys/dev/audio_if.h\n",
		       __func__);
		return;
	}
	memmove(&list->filters[1], &list->filters[0],
		sizeof(struct stream_filter_req) * list->req_size);
	list->filters[0].factory = factory;
	list->filters[0].param = *param;
	list->req_size++;
}
#endif // OLD_FILTER

// audio_enter() を呼んでるのは
// audiobellopen(WR)
// audioopen(WR)
// audioclose(WR)
// audioread(RD)
// audiowrite(RD)
// audioioctl(WR) FLUSH		.. audio_clear/initbufs/start
// audioioctl(WR) SETINFO	.. audiosetinfo
// audioioctl(WR) DRAIN		.. audio_drain
// audioioctl(WR) SETFD		.. get_props/hw->setfd
// audioioctl(RD) それ以外
// audioioctl(RD) GETDEV	.. hw->getdev
// audiommap(RD)
// audiostat は確かにいらなさそうだけど
// poll/kqueue にも元からないようだ
// HW の状態を変えるのを HW への読み書きから保護するのが当初の目的ぽい。
/*
 * Look up audio device and acquire locks for device access.
 */
static int
audio_enter(dev_t dev, struct audio_softc **scp)
{
	struct audio_softc *sc;

	/* First, find the device and take sc_lock. */
	sc = device_lookup_private(&audio_cd, AUDIOUNIT(dev));
	if (sc == NULL || sc->hw_if == NULL)
		return ENXIO;
	mutex_enter(sc->sc_lock);
	if (sc->sc_dying) {
		mutex_exit(sc->sc_lock);
		return EIO;
	}

	*scp = sc;
	return 0;
}

/*
 * Release reference to device acquired with audio_enter().
 */
static void
audio_exit(struct audio_softc *sc)
{
	mutex_exit(sc->sc_lock);
}

/*
 * Wait for I/O to complete, releasing device lock.
 */
// 本当はここに audio_waitio()

// XXX audiobell はここなのか?

/* Exported interfaces for audiobell. */
int
audiobellopen(dev_t dev, int flags, int ifmt, struct lwp *l,
	      struct file **fp)
{
	struct audio_softc *sc;
	int error;

	if ((error = audio_enter(dev, &sc)) != 0)
		return error;
	device_active(sc->dev, DVA_SYSTEM);
	switch (AUDIODEV(dev)) {
	case AUDIO_DEVICE:
		error = audio_open(dev, sc, flags, ifmt, l, fp);
		break;
	default:
		error = EINVAL;
		break;
	}
	audio_exit(sc);

	return error;
}

int
audiobellclose(struct file *fp)
{

	return audioclose(fp);
}

int
audiobellwrite(struct file *fp, off_t *offp, struct uio *uio, kauth_cred_t cred,
	   int ioflag)
{

	return audiowrite(fp, offp, uio, cred, ioflag);
}

int
audiobellioctl(struct file *fp, u_long cmd, void *addr)
{

	return audioioctl(fp, cmd, addr);
}

static int
audioopen(dev_t dev, int flags, int ifmt, struct lwp *l)
{
	struct audio_softc *sc;
	struct file *fp;
	int error;

	if ((error = audio_enter(dev, &sc)) != 0)
		return error;
	device_active(sc->dev, DVA_SYSTEM);
	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
	case AUDIOCTL_DEVICE:
		error = audio_open(dev, sc, flags, ifmt, l, &fp);
		break;
	case MIXER_DEVICE:
		error = mixer_open(dev, sc, flags, ifmt, l, &fp);
		break;
	default:
		error = ENXIO;
		break;
	}
	audio_exit(sc);

	return error;
}

static int
audioclose(struct file *fp)
{
	struct audio_softc *sc;
	audio_file_t *file;
	int error;
	dev_t dev;

	if (fp->f_audioctx == NULL)
		return EIO;	/* XXX:NS Why is this needed. */

	file = (audio_file_t *)fp->f_audioctx;
	dev = file->dev;

	if ((error = audio_enter(dev, &sc)) != 0)
		return error;

	device_active(sc->dev, DVA_SYSTEM);
	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
	case AUDIOCTL_DEVICE:
		error = audio_close(sc, fp->f_flag, file);
		break;
	case MIXER_DEVICE:
		error = mixer_close(sc, fp->f_flag, file);
		break;
	default:
		error = ENXIO;
		break;
	}
	if (error == 0) {
		kmem_free(fp->f_audioctx, sizeof(audio_file_t));
		fp->f_audioctx = NULL;
	}

	audio_exit(sc);

	return error;
}

static int
audioread(struct file *fp, off_t *offp, struct uio *uio, kauth_cred_t cred,
	  int ioflag)
{
	struct audio_softc *sc;
	audio_file_t *file;
	int error;
	dev_t dev;

	if (fp->f_audioctx == NULL)
		return EIO;

	file = (audio_file_t *)fp->f_audioctx;
	dev = file->dev;

	if ((error = audio_enter(dev, &sc)) != 0)
		return error;

	if (fp->f_flag & O_NONBLOCK)
		ioflag |= IO_NDELAY;

	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
		error = audio_read(sc, uio, ioflag, file);
		break;
	case AUDIOCTL_DEVICE:
	case MIXER_DEVICE:
		error = ENODEV;
		break;
	default:
		error = ENXIO;
		break;
	}
	audio_exit(sc);

	return error;
}

static int
audiowrite(struct file *fp, off_t *offp, struct uio *uio, kauth_cred_t cred,
	   int ioflag)
{
	struct audio_softc *sc;
	audio_file_t *file;
	int error;
	dev_t dev;

	if (fp->f_audioctx == NULL)
		return EIO;

	file = (audio_file_t *)fp->f_audioctx;
	dev = file->dev;

	if ((error = audio_enter(dev, &sc)) != 0)
		return error;

	if (fp->f_flag & O_NONBLOCK)
		ioflag |= IO_NDELAY;

	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
		error = audio_write(sc, uio, ioflag, file);
		break;
	case AUDIOCTL_DEVICE:
	case MIXER_DEVICE:
		error = ENODEV;
		break;
	default:
		error = ENXIO;
		break;
	}
	audio_exit(sc);

	return error;
}

static int
audioioctl(struct file *fp, u_long cmd, void *addr)
{
	struct audio_softc *sc;
	audio_file_t *file;
	struct lwp *l = curlwp;
	int error;
	dev_t dev;

	if (fp->f_audioctx == NULL)
		return EIO;

	file = (audio_file_t *)fp->f_audioctx;
	dev = file->dev;

	if ((error = audio_enter(dev, &sc)) != 0)
		return error;

	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
	case AUDIOCTL_DEVICE:
		device_active(sc->dev, DVA_SYSTEM);
		if (IOCGROUP(cmd) == IOCGROUP(AUDIO_MIXER_READ))
			error = mixer_ioctl(sc, cmd, addr, fp->f_flag, l);
		else
			error = audio_ioctl(dev, sc, cmd, addr, fp->f_flag, l,
			    file);
		break;
	case MIXER_DEVICE:
		error = mixer_ioctl(sc, cmd, addr, fp->f_flag, l);
		break;
	default:
		error = ENXIO;
		break;
	}
	audio_exit(sc);

	return error;
}

static int
audiostat(struct file *fp, struct stat *st)
{
	if (fp->f_audioctx == NULL)
		return EIO;

	memset(st, 0, sizeof(*st));

	st->st_dev = ((audio_file_t *)fp->f_audioctx)->dev;
	st->st_uid = kauth_cred_geteuid(fp->f_cred);
	st->st_gid = kauth_cred_getegid(fp->f_cred);
	st->st_mode = S_IFCHR;
	return 0;
}

static int
audiopoll(struct file *fp, int events)
{
	struct audio_softc *sc;
	audio_file_t *file;
	struct lwp *l = curlwp;
	int revents;
	dev_t dev;

	if (fp->f_audioctx == NULL)
		return POLLERR;

	file = (audio_file_t *)fp->f_audioctx;
	dev = file->dev;

	if (audio_enter(dev, &sc) != 0)
		return POLLERR;

	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
		revents = audio_poll(sc, events, l, file);
		break;
	case AUDIOCTL_DEVICE:
	case MIXER_DEVICE:
		revents = 0;
		break;
	default:
		revents = POLLERR;
		break;
	}
	audio_exit(sc);

	return revents;
}

static int
audiokqfilter(struct file *fp, struct knote *kn)
{
	struct audio_softc *sc;
	audio_file_t *file;
	int rv;
	dev_t dev;
	int error;

	if (fp->f_audioctx == NULL)
		return EIO;

	file = (audio_file_t *)fp->f_audioctx;
	dev = file->dev;

	if ((error = audio_enter(dev, &sc)) != 0)
		return error;

	switch (AUDIODEV(file->dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
		rv = audio_kqfilter(sc, file, kn);
		break;
	case AUDIOCTL_DEVICE:
	case MIXER_DEVICE:
		rv = ENODEV;
		break;
	default:
		rv = ENXIO;
		break;
	}
	audio_exit(sc);

	return rv;
}

// これだけ audio_fop_mmap で命名規則おかしかったので変えた
static int
audiommap(struct file *fp, off_t *offp, size_t len, int prot, int *flagsp,
	     int *advicep, struct uvm_object **uobjp, int *maxprotp)
{
	struct audio_softc *sc;
	audio_file_t *file;
	dev_t dev;
	int error;

	if (fp->f_audioctx == NULL)
		return EIO;

	file = (audio_file_t *)fp->f_audioctx;
	dev = file->dev;

	if ((error = audio_enter(dev, &sc)) != 0)
		return error;

	device_active(sc->dev, DVA_SYSTEM); /* XXXJDM */
	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
		error = audio_mmap(sc, offp, len, prot, flagsp, advicep,
		    uobjp, maxprotp, file);	
		break;
	case AUDIOCTL_DEVICE:
	case MIXER_DEVICE:
	default:
		error = ENOTSUP;
		break;
	}
	audio_exit(sc);

	return error;
}


/*
 * Audio driver
 */
int
audio_open(dev_t dev, struct audio_softc *sc, int flags, int ifmt,
    struct lwp *l, struct file **nfp)
{
	struct file *fp;
	audio_file_t *af;
	int fd;
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->hw_if == NULL)
		return ENXIO;

	DPRINTF(("audio_open: flags=0x%x sc=%p hdl=%p\n",
		 flags, sc, sc->hw_hdl));

	af = kmem_zalloc(sizeof(audio_file_t), KM_SLEEP);
	af->sc = sc;
	af->mode = 0;
	if ((flags & FWRITE) != 0 && audio_can_playback(sc))
		af->mode |= AUMODE_PLAY;
	if ((flags & FREAD) != 0 && audio_can_capture(sc))
		af->mode |= AUMODE_RECORD;
	if (af->mode == 0)
		return ENXIO;

	// トラックの初期化
	// トラックバッファの初期化
	if ((af->mode & AUMODE_PLAY) != 0) {
		audio_track_init(&af->ptrack, sc->sc_pmixer, AUMODE_PLAY);
	}
	if ((af->mode & AUMODE_RECORD) != 0) {
		audio_track_init(&af->rtrack, sc->sc_rmixer, AUMODE_RECORD);
	}

	/*
	 * Multiplex device: /dev/audio (MU-Law) and /dev/sound (linear)
	 * The /dev/audio is always (re)set to 8-bit MU-Law mono
	 * For the other devices, you get what they were last set to.
	 */
	if (ISDEVAUDIO(dev)) {
		error = audio_file_set_defaults(sc, af);
	} else {
		struct audio_info ai;

		AUDIO_INITINFO(&ai);
		ai.mode = af->mode;
		error = audio_file_setinfo(sc, af, &ai);
	}
	if (error)
		goto bad;

	// 録再合わせて1本目のオープンなら {
	//	kauth の処理?
	//	hw_if->open
	//	下トラックの初期化?
	// } else if (複数ユーザがオープンすることを許可してなければ) {
	//	チェック?
	// }
	if (sc->sc_popens + sc->sc_ropens == 0) {
		/* First open */
		// kauth?

		if (sc->hw_if->open) {
			mutex_enter(sc->sc_intr_lock);
			error = sc->hw_if->open(sc->hw_hdl, af->mode);
			mutex_exit(sc->sc_intr_lock);
			if (error)
				goto bad;
		}

		/* Set speaker mode if half duplex */
		if (!sc->sc_full_duplex) {
			if (sc->hw_if->speaker_ctl) {
				int on;
				if ((af->mode & AUMODE_PLAY) != 0) {
					on = 1;
				} else {
					on = 0;
				}
				mutex_enter(sc->sc_intr_lock);
				error = sc->hw_if->speaker_ctl(sc->hw_hdl, on);
				mutex_exit(sc->sc_intr_lock);
				if (error)
					goto bad;
			}
		}
	}

	error = fd_allocfile(&fp, &fd);
	if (error)
		goto bad;

	DPRINTF(("audio_open: done sc_mode = 0x%x\n", af->mode));

	// このミキサーどうするか
	//grow_mixer_states(sc, 2);

	// オープンカウント++
	if ((af->mode & AUMODE_PLAY) != 0)
		sc->sc_popens++;
	if ((af->mode & AUMODE_RECORD) != 0)
		sc->sc_ropens++;

	SLIST_INSERT_HEAD(&sc->sc_files, af, entry);

	error = fd_clone(fp, fd, flags, &audio_fileops, af);
	KASSERT(error == EMOVEFD);

	*nfp = fp;
	return error;

bad:
	kmem_free(af, sizeof(*af));
	return error;
}

int __unused
audio_drain(struct audio_softc *sc, audio_track_t *track)
{
	return 0;
}

int
audio_close(struct audio_softc *sc, int flags, audio_file_t *file)
{
	KASSERT(mutex_owned(sc->sc_lock));

	// いる?
	//if (sc->sc_opens == 0 && sc->sc_recopens == 0)
	//	return ENXIO;

	// これが最後の録音トラックなら、halt_input を呼ぶ?
	if (sc->sc_ropens == 1 && (flags & FREAD) != 0) {
		// SB とかいくつかのドライバは halt_input と halt_output に
		// 同じルーチンを使用しているので、その場合は full duplex なら
		// halt_input を呼ばなくする?。ドライバのほうを直すべき。
		/*
		 * XXX Some drivers (e.g. SB) use the same routine
		 * to halt input and output so don't halt input if
		 * in full duplex mode.  These drivers should be fixed.
		 */
		// XXX これはドライバのほうを先に直せばこれだけで済む
		sc->hw_if->halt_input(sc->hw_hdl);

		sc->sc_rbusy = false;
	}

	// 再生トラックなら、audio_drain を呼ぶ
	// 最後の再生トラックなら、hw audio_drain、halt_output を呼ぶ
	if ((flags & FWRITE) != 0) {
		//audio_file_drain(sc);	// ?

		if (sc->sc_popens == 1) {
		}
	}


	// hw->close を呼ぶ
	// cred 解放?
	// リソース解放
	return 0;
}

int
audio_read(struct audio_softc *sc, struct uio *uio, int ioflag,
	audio_file_t *file)
{
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	// いる?
	if (sc->hw_if == NULL)
		return ENXIO;

	if ((file->mode & AUMODE_RECORD) != 0)
		return ENXIO;

	// mmaped なら error

#ifdef AUDIO_PM_IDLE
	if (device_is_active(&sc->dev) || sc->sc_idle)
		device_active(&sc->dev, DVA_SYSTEM);
#endif

	error = 0;
	/*
	 * If hardware is half-duplex and currently playing, return
	 * silence blocks based on the number of blocks we have output.
	 */
	// ハードウェアが half-duplex で現在再生中なら、無音ブロックを返す
	// ここは外部仕様になるので変えない

	// そうでなければリード
	return error;	/* XXX */
}

// 再生中なら再生を停止
// 録音中なら録音を停止 (最後の録音停止時だけ halt_input 呼んでるけど)
void __unused
audio_clear(struct audio_softc *sc)
{
	// clear とは
}

void __unused
audio_clear_intr_unlocked(struct audio_softc *sc)
{
}

// ここに audio_write

// たぶん audio_ioctl はハードウェアレイヤの変更は行わないはずなので
// これ実行中に同時に他の操作が来ても大丈夫?
// AUDIO_FLUSH .. ハードウェアレイヤまで手を出すかどうか
// AUDIO_DRAIN .. どこまで drain するか
// AUDIO_SETFD .. たぶんソフトウェアレイヤで済ませるはず
int
audio_ioctl(dev_t dev, struct audio_softc *sc, u_long cmd, void *addr, int flag,
	    struct lwp *l, audio_file_t *file)
{
	//struct audio_offset *ao;
	int error/*, offs*//*, fd*/;

	KASSERT(mutex_owned(sc->sc_lock));

	// ioctl ターゲットチャンネルを探す
	// XXX SETCHAN,GETCHAN はどうするか

	DPRINTF(("audio_ioctl(%lu,'%c',%lu)\n",
		 IOCPARM_LEN(cmd), (char)IOCGROUP(cmd), cmd&0xff));
	if (sc->hw_if == NULL)
		return ENXIO;
	error = 0;
	switch (cmd) {
#if 0
	case AUDIO_GETCHAN:
		if ((int *)addr != NULL)
			*(int*)addr = chan->chan;
		break;
	case AUDIO_SETCHAN:
		if ((int *)addr != NULL && *(int*)addr > 0)
			chan->deschan = *(int*)addr;
		break;
#endif
	case FIONBIO:
		/* All handled in the upper FS layer. */
		break;

#if 0
	case FIONREAD:
		// 入力バッファにあるバイト数
		*(int *)addr = audio_stream_get_used(vc->sc_rustream);
		break;
#endif

	case FIOASYNC:
		if (*(int *)addr) {
			if (sc->sc_async_audio != 0)
				error = EBUSY;
			else
				sc->sc_async_audio = curproc->p_pid;
			DPRINTF(("audio_ioctl: FIOASYNC pid %d\n",
			    sc->sc_async_audio));
		} else
			sc->sc_async_audio = 0;
		break;

	case AUDIO_FLUSH:
		// このコマンドはすべての再生と録音を停止し、すべてのキューの
		// バッファをクリアし、エラーカウンタをリセットし、そして
		// 現在のサンプリングモードで再生と録音を再開します。
		DPRINTF(("AUDIO_FLUSH\n"));
		printf("AUDIO_FLUSH\n");
		break;

	/*
	 * Number of read (write) samples dropped.  We don't know where or
	 * when they were dropped.
	 */
#if 0
	// サンプル数と言ってるがバイト数のようだ
	case AUDIO_RERROR:
		*(int *)addr = vc->sc_mrr.drops;
		break;

	case AUDIO_PERROR:
		*(int *)addr = vc->sc_mpr.drops;
		break;
#endif

	/*
	 * Offsets into buffer.
	 */
	case AUDIO_GETIOFFS:
	case AUDIO_GETOOFFS:
		// このコマンドはハードウェアの DMA がput(get)しようとしている
		// 入力(出力)バッファでの現在のオフセットを取得します?。
		// これは mmap(2) システムコールによってデバイスバッファが
		// ユーザスペースから見えてる場合に有用です。
		break;

	/*
	 * How many bytes will elapse until mike hears the first
	 * sample of what we write next?
	 */
#if 0
	case AUDIO_WSEEK:
		*(u_long *)addr = audio_stream_get_used(vc->sc_pustream);
		break;
#endif

	case AUDIO_SETINFO:
		DPRINTF(("AUDIO_SETINFO mode=0x%x\n", file->mode));
		error = audio_file_setinfo(sc, file, (struct audio_info *)addr);
		if (error)
			break;
		/* update last_ai if /dev/sound */
		// XXX need_mixerinfo は false にして構わないはず
		if (ISDEVSOUND(dev))
			error = audiogetinfo(sc, &sc->sc_ai, 0, file);
		break;

	case AUDIO_GETINFO:
		DPRINTF(("AUDIO_GETINFO\n"));
		error = audiogetinfo(sc, (struct audio_info *)addr, 1, file);
		break;

	case AUDIO_GETBUFINFO:
		DPRINTF(("AUDIO_GETBUFINFO\n"));
		error = audiogetinfo(sc, (struct audio_info *)addr, 0, file);
		break;

	case AUDIO_DRAIN:
		DPRINTF(("AUDIO_DRAIN\n"));
		printf("AUDIO_DRAIN not implemented\n");
		// audio_drain 呼んで
		// 最後の再生トラックなら hw_if->drain をコールする
		break;

	case AUDIO_GETDEV:
		DPRINTF(("AUDIO_GETDEV\n"));
		error = sc->hw_if->getdev(sc->hw_hdl, (audio_device_t *)addr);
		break;

	case AUDIO_GETENC:
		DPRINTF(("AUDIO_GETENC\n"));
		error = audio_getenc(sc, addr);
		break;

	case AUDIO_GETFD:
		DPRINTF(("AUDIO_GETFD\n"));
		// XXX Full duplex かどうか
		//*(int *)addr = 1;
		break;

	case AUDIO_SETFD:
		DPRINTF(("AUDIO_SETFD\n"));
		//fd = *(int *)addr;
		// XXX 仕様を決める必要がある
		// audio(4)
		// Full-duplex デバイスでは、読み書きは干渉なく同時に行える。
		// full-duplex 可能なデバイスが R/W モードでオープンされた場合
		// play の half-duplex として開始する。full-duplex にするには
		// 明示的にセットが必要。
		break;

	case AUDIO_GETPROPS:
		DPRINTF(("AUDIO_GETPROPS\n"));
		*(int *)addr = audio_get_props(sc);
		break;

	default:
		if (sc->hw_if->dev_ioctl) {
			error = sc->hw_if->dev_ioctl(sc->hw_hdl,
			    cmd, addr, flag, l);
		} else {
			DPRINTF(("audio_ioctl: unknown ioctl\n"));
			error = EINVAL;
		}
		break;
	}
	DPRINTF(("audio_ioctl(%lu,'%c',%lu) result %d\n",
		 IOCPARM_LEN(cmd), (char)IOCGROUP(cmd), cmd&0xff, error));
	return error;
}

int
audio_poll(struct audio_softc *sc, int events, struct lwp *l,
	audio_file_t *file)
{
	int revents;
	//int used;

	KASSERT(mutex_owned(sc->sc_lock));

	DPRINTF(("audio_poll: events=0x%x mode=%d\n", events, file->mode));

	revents = 0;
	if (events & (POLLIN | POLLRDNORM)) {
#if 0 // half duplex のくだりはどうするか
		used = audio_stream_get_used(vc->sc_rustream);
		/*
		 * If half duplex and playing, audio_read() will generate
		 * silence at the play rate; poll for silence being
		 * available.  Otherwise, poll for recorded sound.
		 */
		if ((!vc->sc_full_duplex && (vc->sc_mode & AUMODE_PLAY))
		     ? vc->sc_mpr.stamp > vc->sc_wstamp :
		    used > vc->sc_mrr.usedlow)
			revents |= events & (POLLIN | POLLRDNORM);
#endif
	}

	if (events & (POLLOUT | POLLWRNORM)) {
#if 0 // half duplex のくだりはどうするか
		used = audio_stream_get_used(vc->sc_pustream);
		/*
		 * If half duplex and recording, audio_write() will throw
		 * away play data, which means we are always ready to write.
		 * Otherwise, poll for play buffer being below its low water
		 * mark.
		 */
		if ((!vc->sc_full_duplex && (vc->sc_mode & AUMODE_RECORD)) ||
		    (!(vc->sc_mode & AUMODE_PLAY_ALL) && vc->sc_playdrop > 0) ||
		    (used <= vc->sc_mpr.usedlow))
			revents |= events & (POLLOUT | POLLWRNORM);
#endif
	}

	if (revents == 0) {
		if (events & (POLLIN | POLLRDNORM))
			selrecord(l, &sc->sc_rsel);

		if (events & (POLLOUT | POLLWRNORM))
			selrecord(l, &sc->sc_wsel);
	}

	return revents;
}

static void
filt_audiordetach(struct knote *kn)
{
	struct audio_softc *sc;
	audio_file_t *file;

	file = kn->kn_hook;
	sc = file->sc;

	mutex_enter(sc->sc_intr_lock);
	SLIST_REMOVE(&sc->sc_rsel.sel_klist, kn, knote, kn_selnext);
	mutex_exit(sc->sc_intr_lock);
}

static int
filt_audioread(struct knote *kn, long hint)
{
	struct audio_softc *sc;
	audio_file_t *file;

	file = kn->kn_hook;
	sc = file->sc;

	mutex_enter(sc->sc_intr_lock);
	// XXX なんだこれ
#if 0
	if ((file->mode & AUMODE_RECORD) != 0)
		kn->kn_data = vc->sc_mpr.stamp - vc->sc_wstamp;
	else
		kn->kn_data = audio_stream_get_used(vc->sc_rustream)
			- vc->sc_mrr.usedlow;
#endif
	mutex_exit(sc->sc_intr_lock);

	return kn->kn_data > 0;
}

static const struct filterops audioread_filtops = {
	.f_isfd = 1,
	.f_attach = NULL,
	.f_detach = filt_audiordetach,
	.f_event = filt_audioread,
};

static void
filt_audiowdetach(struct knote *kn)
{
	struct audio_softc *sc;
	audio_file_t *file;

	file = kn->kn_hook;
	sc = file->sc;

	mutex_enter(sc->sc_intr_lock);
	SLIST_REMOVE(&sc->sc_wsel.sel_klist, kn, knote, kn_selnext);
	mutex_exit(sc->sc_intr_lock);
}

static int
filt_audiowrite(struct knote *kn, long hint)
{
	struct audio_softc *sc;
	audio_file_t *file;
	audio_track_t *track;
	audio_ring_t *buf;

	file = kn->kn_hook;
	sc = file->sc;
	track = &file->ptrack;
	buf = &track->outputbuf;

	// XXX WRITE 可能な時しかここ来ないのかな?

	mutex_enter(sc->sc_intr_lock);

	// 再生バッファの空きバイト数を kn_data に入れて、
	// 空きがあるかどうかを返す?
	kn->kn_data = (buf->capacity - buf->count) *
	    (track->inputfmt.stride * track->inputfmt.channels);

	mutex_exit(sc->sc_intr_lock);

	return kn->kn_data > 0;
}

static const struct filterops audiowrite_filtops = {
	.f_isfd = 1,
	.f_attach = NULL,
	.f_detach = filt_audiowdetach,
	.f_event = filt_audiowrite,
};

int
audio_kqfilter(struct audio_softc *sc, audio_file_t *file, struct knote *kn)
{
	struct klist *klist;

	switch (kn->kn_filter) {
	case EVFILT_READ:
		klist = &sc->sc_rsel.sel_klist;
		kn->kn_fop = &audioread_filtops;
		break;

	case EVFILT_WRITE:
		klist = &sc->sc_wsel.sel_klist;
		kn->kn_fop = &audiowrite_filtops;
		break;

	default:
		return EINVAL;
	}

	kn->kn_hook = file;

	mutex_enter(sc->sc_intr_lock);
	SLIST_INSERT_HEAD(klist, kn, kn_selnext);
	mutex_exit(sc->sc_intr_lock);

	return 0;
}

int
audio_mmap(struct audio_softc *sc, off_t *offp, size_t len, int prot,
	int *flagsp, int *advicep, struct uvm_object **uobjp, int *maxprotp,
	audio_file_t *file)
{
	// あとでかんがえる
#if 0
	struct audio_ringbuffer *cb;

	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->hw_if == NULL)
		return ENXIO;

	DPRINTF(("audio_mmap: off=%lld, prot=%d\n", (long long)(*offp), prot));
	if (!(audio_get_props(sc) & AUDIO_PROP_MMAP))
		return ENOTSUP;

	if (*offp < 0)
		return EINVAL;

#if 0
	/* XXX
	 * The idea here was to use the protection to determine if
	 * we are mapping the read or write buffer, but it fails.
	 * The VM system is broken in (at least) two ways.
	 * 1) If you map memory VM_PROT_WRITE you SIGSEGV
	 *    when writing to it, so VM_PROT_READ|VM_PROT_WRITE
	 *    has to be used for mmapping the play buffer.
	 * 2) Even if calling mmap() with VM_PROT_READ|VM_PROT_WRITE
	 *    audio_mmap will get called at some point with VM_PROT_READ
	 *    only.
	 * So, alas, we always map the play buffer for now.
	 */
	if (prot == (VM_PROT_READ|VM_PROT_WRITE) ||
	    prot == VM_PROT_WRITE)
		cb = &vc->sc_mpr;
	else if (prot == VM_PROT_READ)
		cb = &vc->sc_mrr;
	else
		return EINVAL;
#else
	cb = &vc->sc_mpr;
#endif

	if (len > cb->s.bufsize || *offp > (uint)(cb->s.bufsize - len))
		return EOVERFLOW;

	if (!cb->mmapped) {
		cb->mmapped = true;
		if (cb == &vc->sc_mpr) {
			audio_fill_silence(&cb->s.param, cb->s.start,
					   cb->s.bufsize);
			vc->sc_pustream = &cb->s;
			if (!vc->sc_pbus && !vc->sc_mpr.pause)
				(void)audiostartp(sc, vc);
		} else if (cb == &vc->sc_mrr) {
			vc->sc_rustream = &cb->s;
			if (!vc->sc_rbus && !sc->sc_mixring.sc_mrr.pause)
				(void)audiostartr(sc, vc);
		}
	}

	/* get ringbuffer */
	*uobjp = cb->uobj;

	/* Acquire a reference for the mmap.  munmap will release.*/
	uao_reference(*uobjp);
	*maxprotp = prot;
	*advicep = UVM_ADV_RANDOM;
	*flagsp = MAP_SHARED;
#endif
	return 0;
}

// audiostart{p,r} の呼び出し元
//  audio_resume
//  audio_drain		.. 残ってるのを全部吐き出すためにいるかも
//  audio_read
//  audio_write
//  audio_ioctl FLUSH
//  audio_mmap		.. mmapした途端に動き出すやつ
//  audio_setinfo_hw .. 一旦とめたやつの再開
int
audiostartr(struct audio_softc *sc)
{
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	DPRINTF(("audiostartr\n"));

	if (!audio_can_capture(sc))
		return EINVAL;

	// ここで録音ループ起動
	sc->sc_rbusy = true;

	return error;
}

int
audiostartp(struct audio_softc *sc)
{
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	error = 0;
	DPRINTF(("audiostartp\n"));

	if (!audio_can_playback(sc))
		return EINVAL;

	// ここで再生ループ起動
	sc->sc_pbusy = true;

	return error;
}

// audio_pint_silence いるかどうかは今後

static void
audio_softintr_rd(void *cookie)
{
	struct audio_softc *sc = cookie;
	proc_t *p;
	pid_t pid;

	mutex_enter(sc->sc_lock);
	cv_broadcast(&sc->sc_rchan);
	selnotify(&sc->sc_rsel, 0, NOTE_SUBMIT);
	if ((pid = sc->sc_async_audio) != 0) {
		DPRINTFN(3, ("audio_softintr_rd: sending SIGIO %d\n", pid));
		mutex_enter(proc_lock);
		if ((p = proc_find(pid)) != NULL)
			psignal(p, SIGIO);
		mutex_exit(proc_lock);
	}
	mutex_exit(sc->sc_lock);
}

static void
audio_softintr_wr(void *cookie)
{
	struct audio_softc *sc = cookie;
	proc_t *p;
	pid_t pid;

	mutex_enter(sc->sc_lock);
	cv_broadcast(&sc->sc_wchan);
	selnotify(&sc->sc_wsel, 0, NOTE_SUBMIT);
	if ((pid = sc->sc_async_audio) != 0) {
		DPRINTFN(3, ("audio_softintr_wr: sending SIGIO %d\n", pid));
		mutex_enter(proc_lock);
		if ((p = proc_find(pid)) != NULL)
			psignal(p, SIGIO);
		mutex_exit(proc_lock);
	}
	mutex_exit(sc->sc_lock);
}

/*
 * Called from HW driver module on completion of DMA output.
 * Start output of new block, wrap in ring buffer if needed.
 * If no more buffers to play, output zero instead.
 * Do a wakeup if necessary.
 */
void __unused
audio_pintr(void *v)
{
	struct audio_softc *sc;

	sc = v;

	// 次のループを回す
	// XXX カウントとは
	audio_trackmixer_intr(sc->sc_pmixer, 1/*count?*/);
}

/*
 * Called from HW driver module on completion of DMA input.
 * Mark it as input in the ring buffer (fiddle pointers).
 * Do a wakeup if necessary.
 */
void __unused
audio_rintr(void *v)
{
#if 0	// XXX not yet
	struct audio_softc *sc;

	sc = v;

	//audio_trackmixer_rintr(sc, 1/*count?*/);

	// 次のループを回す
#endif
}

// SLINEAR -> SLINEAR_NE
// {U,S}LINEAR8_* は {U,S}LINEAR8_LE を代表値として使う
//
// 以下から呼ばれる:
// audio_setup_pfilters(to-param) 戻り値は見てない
// audio_setup_rfilters(from-param) 戻り値は見てない
// audiosetinfo: 設定値に対して、エラーならreturn
// audiosetinfo: audio_set_params()コール後に対して、戻り値見てない
static int
audio_check_params(struct audio_params *p)
{

	if (p->encoding == AUDIO_ENCODING_PCM16) {
		if (p->precision == 8)
			p->encoding = AUDIO_ENCODING_ULINEAR;
		else
			p->encoding = AUDIO_ENCODING_SLINEAR;
	} else if (p->encoding == AUDIO_ENCODING_PCM8) {
		if (p->precision == 8)
			p->encoding = AUDIO_ENCODING_ULINEAR;
		else
			return EINVAL;
	}

	if (p->encoding == AUDIO_ENCODING_SLINEAR)
		p->encoding = AUDIO_ENCODING_SLINEAR_NE;
	if (p->encoding == AUDIO_ENCODING_ULINEAR)
		p->encoding = AUDIO_ENCODING_ULINEAR_NE;

	switch (p->encoding) {
	case AUDIO_ENCODING_ULAW:
	case AUDIO_ENCODING_ALAW:
		if (p->precision != 8)
			return EINVAL;
		break;
	case AUDIO_ENCODING_ADPCM:
		if (p->precision != 4 && p->precision != 8)
			return EINVAL;
		break;
	case AUDIO_ENCODING_SLINEAR_LE:
	case AUDIO_ENCODING_SLINEAR_BE:
	case AUDIO_ENCODING_ULINEAR_LE:
	case AUDIO_ENCODING_ULINEAR_BE:
		/* XXX is: our zero-fill can handle any multiple of 8 */
		if (p->precision !=  8 && p->precision != 16 &&
		    p->precision != 24 && p->precision != 32)
			return EINVAL;
		if (p->precision == 8 && p->encoding ==
		    AUDIO_ENCODING_SLINEAR_BE)
			p->encoding = AUDIO_ENCODING_SLINEAR_LE;
		if (p->precision == 8 && p->encoding ==
		    AUDIO_ENCODING_ULINEAR_BE)
			p->encoding = AUDIO_ENCODING_ULINEAR_LE;
		if (p->validbits > p->precision)
			return EINVAL;
		break;
	case AUDIO_ENCODING_MPEG_L1_STREAM:
	case AUDIO_ENCODING_MPEG_L1_PACKETS:
	case AUDIO_ENCODING_MPEG_L1_SYSTEM:
	case AUDIO_ENCODING_MPEG_L2_STREAM:
	case AUDIO_ENCODING_MPEG_L2_PACKETS:
	case AUDIO_ENCODING_MPEG_L2_SYSTEM:
	case AUDIO_ENCODING_AC3:
		break;
	default:
		return EINVAL;
	}

	/* sanity check # of channels*/
	if (p->channels < 1 || p->channels > AUDIO_MAX_CHANNELS)
		return EINVAL;

	return 0;
}

static int
audio_check_params2(const audio_format2_t *p2)
{
	struct audio_params p;
	p = format2_to_params(p2);
	return audio_check_params(&p);
}

// 周波数を選択する。
// XXX アルゴリズムどうすっかね
// 48kHz、44.1kHz をサポートしてれば優先して使用。
// そうでなければとりあえず最高周波数にしとくか。
static int
xxx_select_freq(const struct audio_format *fmt)
{
	int freq;
	int j;

	if (fmt->frequency_type == 0) {
		int low = fmt->frequency[0];
		int high = fmt->frequency[1];
		freq = 48000;
		if (low <= freq && freq <= high) {
			return freq;
		}
		freq = 44100;
		if (low <= freq && freq <= high) {
			return freq;
		}
		return high;
	} else {
		freq = 48000;
		for (j = 0; j < fmt->frequency_type; j++) {
			if (fmt->frequency[j] == freq) {
				return freq;
			}
		}
		freq = 44100;
		for (j = 0; j < fmt->frequency_type; j++) {
			if (fmt->frequency[j] == freq) {
				return freq;
			}
		}
		return fmt->frequency[fmt->frequency_type - 1];
	}
}

// 再生か録音(mode) の HW フォーマットを決定する
static int
xxx_config_hwfmt(struct audio_softc *sc, audio_format2_t *cand, int mode)
{
	const struct audio_format *formats;
	int nformats;
	int i;

	// 初期値
	// enc/prec/stride は固定。(XXX 逆エンディアン対応するか?)
	// channels/frequency はいいやつを選びたい
	cand->encoding    = AUDIO_ENCODING_SLINEAR_NE;
	cand->precision   = AUDIO_INTERNAL_BITS;
	cand->stride      = AUDIO_INTERNAL_BITS;
	cand->channels    = 1;
	cand->sample_rate = 0;	// 番兵

	mutex_enter(sc->sc_lock);
	nformats = sc->hw_if->query_format(sc->hw_hdl, &formats);
	mutex_exit(sc->sc_lock);
	if (nformats == 0)
		return ENXIO;
	for (i = 0; i < nformats; i++) {
		const struct audio_format *fmt = &formats[i];

		if (!AUFMT_IS_VALID(fmt)) {
			printf("fmt[%d] skip; INVALID\n", i);
			continue;
		}
		if ((fmt->mode & mode) == 0) {
			printf("fmt[%d] skip; mode not match %d\n", i, mode);
			continue;
		}

		if (fmt->encoding != AUDIO_ENCODING_SLINEAR_NE) {
			printf("fmt[%d] skip; enc=%d\n", i, fmt->encoding);
			continue;
		}
		if (fmt->precision != AUDIO_INTERNAL_BITS ||
		    fmt->validbits != AUDIO_INTERNAL_BITS) {
			printf("fmt[%d] skip; precision %d/%d\n", i,
			    fmt->validbits, fmt->precision);
			continue;
		}
		if (fmt->channels < cand->channels) {
			printf("fmt[%d] skip; channels %d < %d\n", i,
			    fmt->channels, cand->channels);
			continue;
		}
		int freq = xxx_select_freq(fmt);
		// XXX うーん
		if (freq < cand->sample_rate) {
			printf("fmt[%d] skip; frequency %d < %d\n", i,
			    freq, cand->sample_rate);
			continue;
		}

		// cand 更新
		cand->channels = fmt->channels;
		cand->sample_rate = freq;
		printf("fmt[%d] cand ch=%d freq=%d\n", i,
		    cand->channels, cand->sample_rate);
	}

	if (cand->sample_rate == 0) {
		printf("%s no fmt\n", __func__);
		return ENXIO;
	}
	printf("%s selected: ch=%d freq=%d\n", __func__,
	    cand->channels, cand->sample_rate);
	return 0;
}

// sc_playback/capture に応じて再生か録音のフォーマットを選択して設定する。
// だめだった方は false にする。
//
// independent デバイスなら
//  あれば再生フォーマットを選定
//  あれば録音フォーマットを選定
//  両方を設定
// not independent デバイスなら
//  再生があれば
//   再生でフォーマットを選定。録音側にもコピー
//  else
//   録音でフォーマットを選定。再生側にもコピー
//  両方を設定
static int
audio_xxx_config(struct audio_softc *sc, int is_indep)
{
	audio_format2_t fmt;
	int mode;
	int error;

	if (is_indep) {
		/* independent devices */
		if (sc->sc_can_playback) {
			error = xxx_config_hwfmt(sc, &sc->sc_phwfmt,
			    AUMODE_PLAY);
			if (error)
				sc->sc_can_playback = false;
		}
		if (sc->sc_can_capture) {
			error = xxx_config_hwfmt(sc, &sc->sc_rhwfmt,
			    AUMODE_RECORD);
			if (error)
				sc->sc_can_capture = false;
		}
	} else {
		/* not independent devices */
		if (sc->sc_can_playback) {
			mode = AUMODE_PLAY;
		} else {
			mode = AUMODE_RECORD;
		}
		error = xxx_config_hwfmt(sc, &fmt, mode);
		if (error) {
			sc->sc_can_playback = false;
			sc->sc_can_capture = false;
			return error;
		}
		sc->sc_phwfmt = fmt;
		sc->sc_rhwfmt = fmt;
	}

	error = audio_set_params(sc);
	if (error)
		return error;

	return error;
}

#define MAX_ENCODINGS	(16)	/* hw(max:10) + emulated(current:6) */

/* Encodings supported by audio layer */
static const audio_encoding_t audio_stdXXX_encodings[] = {
#define EMULATED AUDIO_ENCODINGFLAG_EMULATED
	{ 0, AudioEulinear_le, AUDIO_ENCODING_ULINEAR_LE, 8,  EMULATED },
	{ 1, AudioEslinear_le, AUDIO_ENCODING_SLINEAR_LE, 16, EMULATED },
	{ 2, AudioEslinear_le, AUDIO_ENCODING_SLINEAR_LE, 24, EMULATED },
	{ 3, AudioEslinear_le, AUDIO_ENCODING_SLINEAR_LE, 32, EMULATED },
	{ 4, AudioEmulaw,      AUDIO_ENCODING_ULAW, 8, EMULATED },
	{ 5, AudioEalaw,       AUDIO_ENCODING_ALAW, 8, EMULATED },
#undef EMULATED
};

void __unused
audio_prepare_enc_xxx(struct audio_softc *sc)
{
	struct audio_encoding enc;
	struct audio_encoding matched;
	int idx;
	int dst;
	int i;
	int hwmax;
	int stdmax;
	int error;
	bool found;

	stdmax = __arraycount(audio_stdXXX_encodings);

	/* Find preferrable encoding とついでに数を数える */
	memset(&matched, 0, sizeof(matched));
	for (idx = 0; ; idx++) {
		memset(&enc, 0, sizeof(enc));
		enc.index = idx;
		error = sc->hw_if->query_encoding(sc->hw_hdl, &enc);
		if (error)
			break;

		if (enc.encoding == AUDIO_ENCODING_SLINEAR_NE &&
		    enc.precision == AUDIO_INTERNAL_BITS &&
		    matched.encoding == 0 /* first match */) {
			matched = enc;
		}
	}
	hwmax = idx;


	/*
	 * Prepare sc_encodings for AUDIO_GETENC.
	 */
	sc->sc_encodings = kmem_zalloc((stdmax + hwmax) *
	    sizeof(struct audio_encoding), KM_SLEEP);

	/* Copy standard encodings first */
	memcpy(sc->sc_encodings, &audio_stdXXX_encodings,
	    sizeof(audio_stdXXX_encodings));

	/* Enumerates encodings supported by hw driver */
	for (idx = 0, dst = stdmax; idx < hwmax; idx++) {
		memset(&enc, 0, sizeof(enc));
		enc.index = idx;
		error = sc->hw_if->query_encoding(sc->hw_hdl, &enc);
		if (error)
			break;

		found = false;
		for (i = 0; i < stdmax; i++) {
			/* Clear EMULATED flag if hw supports it natively */
			if (enc.encoding == sc->sc_encodings[i].encoding &&
			    enc.precision == sc->sc_encodings[i].precision &&
			    enc.flags == 0) {
				sc->sc_encodings[i].flags = 0;
				found = true;
			}
		}
		if (!found) {
			/* Otherwise, append */
			memcpy(&sc->sc_encodings[dst], &enc, sizeof(enc));
			dst++;
		}
	}
	sc->sc_encodings_count = dst;
}

// この audio_file を /dev/audio のデフォルト値(mulaw)に設定する
static int
audio_file_set_defaults(struct audio_softc *sc, audio_file_t *file)
{
	struct audio_info ai;

	KASSERT(mutex_owned(sc->sc_lock));

	AUDIO_INITINFO(&ai);
	if ((file->mode & AUMODE_PLAY) != 0) {
		ai.play.sample_rate   = audio_default.sample_rate;
		ai.play.encoding      = audio_default.encoding;
		ai.play.channels      = audio_default.channels;
		ai.play.precision     = audio_default.precision;
		ai.play.pause         = false;
	}
	if ((file->mode & AUMODE_RECORD) != 0) {
		ai.record.sample_rate = audio_default.sample_rate;
		ai.record.encoding    = audio_default.encoding;
		ai.record.channels    = audio_default.channels;
		ai.record.precision   = audio_default.precision;
		ai.record.pause	      = false;
	}
	ai.mode = file->mode & (AUMODE_PLAY | AUMODE_RECORD);

	return audio_file_setinfo(sc, file, &ai);
}

// ai の値を反映させる。
//
// ai.{play,record}.sample_rate
// ai.{play,record}.encoding
// ai.{play,record}.precision
// ai.{play,record}.channels
//	再生/録音パラメータ。
//	現在有効でない側は無視する。sc的にではなくこのfd的に有効かどうかでは?
//	通常は Upper Lane のみ。将来的に COOKED モードなら HW のみ。
//
// ai.mode			.. 再生/録音のどちらのモードか。
// ai.{hiwat,lowat}
//	これは Upper Lane
//
// ai.{play,record}.gain	.. ソフトウェアボリューム
//	これは Upper Lane。(N8 以降)
//
// ai.{play,record}.balance	.. 左右バランス
//	現在はハードウェア(ミキサー)だが、将来的には UpperLane にすべき。
//
// ai.{play,record}.port	.. 入出力ポート
// ai.monitor_gain			.. 録音モニターゲイン?
//	これはハードウェア(ミキサー)
//
// ai.{play,record}.pause	.. 一時停止?
//	Upper Lane とハードウェア両方??
//	何をどこまで一時停止するかによる?
//
//
#if 0
	// HALF-DUPLEX
	// 1本目:
	//	PLAY -> play
	//	REC -> rec
	//	PLAY|REC -> play
	// 2本目:
	//	1本目がplay: PLAY -> play
	//	1本目がplay: REC  -> ENODEV?
	//	1本目がplay: PLAY|REC -> PLAY -> play
	//	1本目がrec:  PLAY -> ENODEV?
	//	1本目がrec:  REC  -> rec
	//	1本目がrec:  PLAY|REC -> PLAY -> ENODEV?
	//
	// 再生専用デバイス
	// 1本目:
	//	PLAY -> play
	//	REC -> ENODEV?
	//	PLAY|REC -> PLAY -> play
	//
	// 録音専用デバイス
	// 1本目:
	//	PLAY -> ENODEV?
	//	REC -> rec
	//	PLAY|REC -> ENODEV?
	//

	if (SPECIFIED(ai->mode)) {
		modechange = true;
		mode = ai->mode;
		/* sanity check */
		if ((mode & AUMODE_PLAY_ALL) != 0)
			mode |= AUMODE_PLAY;
		/* XXX ここらへんどうするか */
		if ((mode & AUMODE_PLAY) != 0 && sc->sc_full_duplex)
			/* Play takes precedence */
			mode &= ~AUMODE_RECORD;
	}
#endif

static int
audio_file_setinfo(struct audio_softc *sc, audio_file_t *file,
	const struct audio_info *ai)
{
	const struct audio_prinfo *p;
	const struct audio_prinfo *r;
	audio_track_t *play;
	audio_track_t *rec;
	audio_format2_t pfmt;
	audio_format2_t rfmt;
	int pchanges;
	int rchanges;
	int mode;
	struct audio_info bk;
	audio_format2_t saved_pfmt;
	audio_format2_t saved_rfmt;
	int saved_pvolume;
	int saved_rvolume;
	int saved_ppause;
	int saved_rpause;
	int error;

	p = &ai->play;
	r = &ai->record;
	play = NULL;
	rec = NULL;
	pchanges = 0;
	rchanges = 0;

	if ((file->mode & AUMODE_PLAY) != 0)
		play = &file->ptrack;
	if ((file->mode & AUMODE_RECORD) != 0)
		rec = &file->rtrack;

	/* XXX shut up gcc */
	memset(&saved_pfmt, 0, sizeof(saved_pfmt));
	saved_pvolume = 0;
	saved_ppause = 0;
	memset(&saved_rfmt, 0, sizeof(saved_rfmt));
	saved_rvolume = 0;
	saved_rpause = 0;

	/* Save current parameters */
	if (play) {
		saved_pfmt = play->inputfmt;
		saved_pvolume = play->volume;
		saved_ppause = play->is_pause;
	}
	if (rec) {
		saved_rfmt = rec->outputbuf.fmt;
		saved_rvolume = rec->volume;
		saved_rpause = rec->is_pause;
	}

	/* Set default value */
	pfmt.sample_rate = sc->sc_ai.play.sample_rate;
	pfmt.channels    = sc->sc_ai.play.channels;
	pfmt.encoding    = sc->sc_ai.play.encoding;
	pfmt.precision   = sc->sc_ai.play.precision;
	rfmt.sample_rate = sc->sc_ai.record.sample_rate;
	rfmt.channels    = sc->sc_ai.record.channels;
	rfmt.encoding    = sc->sc_ai.record.encoding;
	rfmt.precision   = sc->sc_ai.record.precision;
	// XXX とりあえずね
	pfmt.stride      = pfmt.precision;
	rfmt.stride      = rfmt.precision;

	/* Overwrite if specified */
	mode = (file->mode & (AUMODE_PLAY | AUMODE_RECORD));
	if (SPECIFIED(ai->mode)) {
		mode = ai->mode;
		if ((mode & AUMODE_PLAY_ALL) != 0)
			mode |= AUMODE_PLAY;
		// 一本目ならそうかもしれないが
		// 二本目以降はどうしたものか
		if ((mode & AUMODE_PLAY) != 0 && !sc->sc_full_duplex)
			/* Play takes precedence */
			mode &= ~AUMODE_RECORD;
	}
	if (play && (mode & AUMODE_PLAY) != 0) {
		pchanges = audio_file_setinfo_check(&pfmt, p);
		if (pchanges == -1)
			return EINVAL;
	}
	if (rec && (mode & AUMODE_RECORD) != 0) {
		rchanges = audio_file_setinfo_check(&rfmt, r);
		if (rchanges == -1)
			return EINVAL;
	}

	if (SPECIFIED(ai->mode) || pchanges || rchanges) {
		// XXX どこまでどうなる?
		audio_clear_intr_unlocked(sc);
#ifdef AUDIO_DEBUG
		printf("setting mode to %d (pchanges=%d rchanges=%d)\n",
		    mode, pchanges, rchanges);
		if (pchanges)
			audio_print_format2("setting play mode:", &pfmt);
		if (rchanges)
			audio_print_format2("setting rec  mode:", &rfmt);
#endif
	}

	/* Set */
	error = 0;
	if (pchanges) {
		error = audio_file_setinfo_set(play, &pfmt, p, pchanges);
		if (error)
			goto abort1;
	}
	if (rchanges) {
		error = audio_file_setinfo_set(rec, &rfmt, r, rchanges);
		if (error)
			goto abort2;
	}

	file->mode = mode;
	return 0;

	/* Rollback */
abort2:
	AUDIO_INITINFO(&bk);
	bk.record.gain = saved_rvolume;
	bk.record.pause = saved_rpause;
	audio_file_setinfo_set(rec, &saved_rfmt, &bk.record, true);
abort1:
	if (play) {
		AUDIO_INITINFO(&bk);
		bk.play.gain = saved_pvolume;
		bk.play.pause = saved_ppause;
		audio_file_setinfo_set(play, &saved_pfmt, &bk.play, true);
	}
	return error;
}

// info のうち SPECIFIED なパラメータを抜き出して fmt に書き出す。
// 戻り値は 1なら変更あり、0なら変更なし、負数ならエラー(EINVAL)
static int
audio_file_setinfo_check(audio_format2_t *fmt, const struct audio_prinfo *info)
{
	int changes;

	changes = 0;
	if (SPECIFIED(info->sample_rate)) {
		fmt->sample_rate = info->sample_rate;
		changes = 1;
	}
	if (SPECIFIED(info->encoding)) {
		fmt->encoding = info->encoding;
		changes = 1;
	}
	if (SPECIFIED(info->precision)) {
		fmt->precision = info->precision;
		/* we don't have API to specify stride */
		fmt->stride = info->precision;
		changes = 1;
	}
	if (SPECIFIED(info->channels)) {
		fmt->channels = info->channels;
		changes = 1;
	}

	if (changes) {
		if (audio_check_params2(fmt) != 0)
			return -1;
	}

	return changes;
}

// modechange なら track に fmt を設定する。
// あと(modechangeに関わらず) info のソフトウェアパラメータも設定する。
// 成功すれば0、失敗なら errno
// ここではロールバックしない。
static int
audio_file_setinfo_set(audio_track_t *track, audio_format2_t *fmt,
	const struct audio_prinfo *info, bool modechange)
{

	if (modechange)
		audio_track_set_format(track, fmt);

	if (SPECIFIED(info->gain)) {
		if (info->gain > 255)
			return EINVAL;
		track->volume = info->gain;
	}
	if (SPECIFIED_CH(info->pause)) {
		track->is_pause = info->pause;
	}

	return 0;
}

// ai のうちハードウェア設定部分を担当する。
// 途中でエラーが起きると(できるかぎり)ロールバックする。
/*
 * Set only hardware part from *ai.
 * Once error has occured, rollback all settings as possible as I can.
 */
static int
audio_setinfo_hw(struct audio_softc *sc, struct audio_info *ai)
{
	struct audio_prinfo *p;
	struct audio_prinfo *r;
	int pbusy;
	int rbusy;
	u_int pport;
	u_int rport;
	u_int pgain;
	u_int rgain;
	u_char pbalance;
	u_char rbalance;
	int monitor_gain;
	bool cleared;
	int error;

	p = &ai->play;
	r = &ai->record;
	pbusy = sc->sc_pbusy;
	rbusy = sc->sc_rbusy;
	cleared = false;
	error = 0;

	/* XXX shut up gcc */
	pport = 0;
	rport = 0;

	if (SPECIFIED(p->port) || SPECIFIED(r->port)) {
		audio_clear_intr_unlocked(sc);
		cleared = true;
	}
	if (SPECIFIED(p->port)) {
		pport = au_get_port(sc, &sc->sc_outports);
		error = au_set_port(sc, &sc->sc_outports, p->port);
		if (error)
			goto abort1;
	}
	if (SPECIFIED(r->port)) {
		rport = au_get_port(sc, &sc->sc_inports);
		error = au_set_port(sc, &sc->sc_inports, r->port);
		if (error)
			goto abort2;
	}

	if (SPECIFIED_CH(p->balance)) {
		au_get_gain(sc, &sc->sc_outports, &pgain, &pbalance);
		error = au_set_gain(sc, &sc->sc_outports, pgain, p->balance);
		if (error)
			goto abort3;
	}
	if (SPECIFIED_CH(r->balance)) {
		au_get_gain(sc, &sc->sc_inports, &rgain, &rbalance);
		error = au_set_gain(sc, &sc->sc_inports, rgain, r->balance);
		if (error)
			goto abort4;
	}

	if (SPECIFIED(ai->monitor_gain) && sc->sc_monitor_port != -1) {
		monitor_gain = au_get_monitor_gain(sc);
		error = au_set_monitor_gain(sc, ai->monitor_gain);
		if (error)
			goto abort5;
	}

	/* Restart if necessary */
	if (cleared) {
		if (pbusy)
			audiostartp(sc);
		if (rbusy)
			audiostartr(sc);
	}

	// XXX hw の変更はなくていいのでは
	//sc->sc_ai = *ai;
	return 0;

	/* Rollback as possible as it can */
abort5:
	if (SPECIFIED(ai->monitor_gain)) {
		if (monitor_gain != -1)
			au_set_monitor_gain(sc, monitor_gain);
	}
abort4:
	if (SPECIFIED_CH(r->balance))
		au_set_gain(sc, &sc->sc_inports, rgain, rbalance);
abort3:
	if (SPECIFIED_CH(p->balance))
		au_set_gain(sc, &sc->sc_outports, pgain, pbalance);
abort2:
	if (SPECIFIED(r->port))
		au_set_port(sc, &sc->sc_inports, rport);
abort1:
	if (SPECIFIED(p->port))
		au_set_port(sc, &sc->sc_outports, pport);
	if (cleared) {
		if (pbusy)
			audiostartp(sc);
		if (rbusy)
			audiostartr(sc);
	}

	return error;
}

// HW に対する set_param あたり。
// setmode に PLAY_ALL は立っていない。
// indepでないデバイスなら
//  - pp, rp は同じ値がすでにセットされている。
// pp, rp は in/out parameter ではなくす。
// hw->set_params は値を変更できない。(無視する)
static int
audio_set_params(struct audio_softc *sc)
{
	audio_params_t pp, rp;
	int error;
	int setmode;
	int usemode;
#ifdef OLD_FILTER
	stream_filter_list_t pfilters, rfilters;
#endif

	setmode = 0;
	if (sc->sc_can_playback)
		setmode |= AUMODE_PLAY;
	if (sc->sc_can_capture)
		setmode |= AUMODE_RECORD;

	usemode = setmode;
	pp = format2_to_params(&sc->sc_phwfmt);
	rp = format2_to_params(&sc->sc_rhwfmt);

	// XXX HWフィルタ
#ifdef OLD_FILTER
	memset(&pfilters, 0, sizeof(pfilters));
	memset(&rfilters, 0, sizeof(rfilters));
	pfilters.append = stream_filter_list_append;
	pfilters.prepend = stream_filter_list_prepend;
	pfilters.set = stream_filter_list_set;
	rfilters.append = stream_filter_list_append;
	rfilters.prepend = stream_filter_list_prepend;
	rfilters.set = stream_filter_list_set;
#endif

	mutex_enter(sc->sc_lock);
	error = sc->hw_if->set_params(sc->hw_hdl, setmode, usemode,
	    &pp, &rp, &pfilters, &rfilters);
	if (error) {
		mutex_exit(sc->sc_lock);
		DPRINTF(("%s: set_params failed with %d\n",
		    __func__, error));
		return error;
	}

	if (sc->hw_if->commit_settings) {
		error = sc->hw_if->commit_settings(sc->hw_hdl);
		if (error) {
			mutex_exit(sc->sc_lock);
			DPRINTF(("%s: commit_settings failed with %d\n",
			    __func__, error));
			return error;
		}
	}
	mutex_exit(sc->sc_lock);

	/* construct new filter chain */
	// XXX

	DPRINTF(("%s: filter setup is completed.\n", __func__));

	return 0;
}

static int
audiogetinfo(struct audio_softc *sc, struct audio_info *ai, int need_mixerinfo,
	audio_file_t *file)
{
	const struct audio_hw_if *hw;
	struct audio_prinfo *r, *p;
	audio_track_t *ptrack;
	audio_track_t *rtrack;
	int gain;

	KASSERT(mutex_owned(sc->sc_lock));

	r = &ai->record;
	p = &ai->play;
	hw = sc->hw_if;
	if (hw == NULL)		/* HW has not attached */
		return ENXIO;

	ptrack = NULL;
	rtrack = NULL;
	if ((file->mode & AUMODE_PLAY) != 0)
		ptrack = &file->ptrack;
	if ((file->mode & AUMODE_RECORD) != 0)
		rtrack = &file->rtrack;

	memset(&ai, 0, sizeof(ai));

	// XXX 常に inputfmt が埋まってれば if いらない
	if (ptrack) {
		p->sample_rate = ptrack->inputfmt.sample_rate;
		p->channels    = ptrack->inputfmt.channels;
		p->precision   = ptrack->inputfmt.precision;
		p->encoding    = ptrack->inputfmt.encoding;
	} else if (ISDEVSOUND(file->dev)) {
		/* fill with /dev/sound's default */
		p->sample_rate = sc->sc_ai.play.sample_rate;
		p->channels    = sc->sc_ai.play.channels;
		p->precision   = sc->sc_ai.play.precision;
		p->encoding    = sc->sc_ai.play.encoding;
	} else {
		/* fill with /dev/audio's default */
		p->sample_rate = audio_default.sample_rate;
		p->channels    = audio_default.channels;
		p->precision   = audio_default.precision;
		p->encoding    = audio_default.encoding;
	}
	if (rtrack) {
		r->sample_rate = rtrack->outputbuf.fmt.sample_rate;
		r->channels    = rtrack->outputbuf.fmt.channels;
		r->precision   = rtrack->outputbuf.fmt.precision;
		r->encoding    = rtrack->outputbuf.fmt.encoding;
	} else if (ISDEVSOUND(file->dev)) {
		/* fill with /dev/sound's default */
		r->sample_rate = sc->sc_ai.record.sample_rate;
		r->channels    = sc->sc_ai.record.channels;
		r->precision   = sc->sc_ai.record.precision;
		r->encoding    = sc->sc_ai.record.encoding;
	} else {
		/* fill with /dev/audio's default */
		r->sample_rate = audio_default.sample_rate;
		r->channels    = audio_default.channels;
		r->precision   = audio_default.precision;
		r->encoding    = audio_default.encoding;
	}

	// audio(4) より
	// The seek and samples fields are only used by AUDIO_GETINFO and
	// AUDIO_GETBUFINFO.  seek represents the count of samples pending;
	// samples represents the total number of bytes recorded or played,
	// less those that were dropped due to inadequate
	// consumption/production rates.
	// seek と samples のフィールドは AUDIO_GETINFO と AUDIO_GETBUFINFO
	// で使われます(セットは出来ないということ)。
	// seek はペンディングになっているサンプル数を示します。
	// samples は録音再生されたトータルバイト数を示します。
	// ...

	if (ptrack) {
		// たぶんバッファ中の現在位置でいいんじゃないかなあ
		// 入力エンコーディングバイト数換算
		p->seek = ptrack->outputbuf.count *
		    (ptrack->inputfmt.stride * ptrack->inputfmt.channels);
		// XXX カウンタそのままでよさそう
		p->samples = ptrack->inputcounter;
		p->eof = sc->sc_eof;
		p->pause = ptrack->is_pause;
		p->error = 0;			// XXX
		p->waiting = 0;			/* open never hangs */
		p->open = 1;
		p->active = sc->sc_pmixer->busy;// XXX ?
		// XXX 入力エンコーディング換算
		p->buffer_size = ptrack->outputbuf.capacity *
		    (ptrack->inputfmt.stride * ptrack->inputfmt.channels);
	}
	if (rtrack) {
		// たぶんバッファ中の現在位置でいいんじゃないかなあ
		// 入力エンコーディングバイト数換算
		r->seek = rtrack->outputbuf.count *
		    (rtrack->outputbuf.fmt.stride * ptrack->outputbuf.fmt.channels);
		// XXX カウンタそのままでよさそう
		r->samples = rtrack->outputcounter;
		r->eof = sc->sc_eof;
		r->pause = rtrack->is_pause;
		r->error = 0;			// XXX
		r->waiting = 0;			/* open never hangs */
		r->open = 1;
		r->active = sc->sc_rmixer->busy;// XXX ?
		r->buffer_size = rtrack->outputbuf.capacity *
		    (rtrack->outputbuf.fmt.stride * rtrack->outputbuf.fmt.channels);
	}

	// XXX 再生と録音でチャンネル数が違う場合があるので
	// ブロックサイズは同じにはならないのだが、
	// もう仕方ないので異なる場合は再生側で代表するか?
	audio_trackmixer_t *mixer;
	audio_format2_t *fmt;
	if (ptrack == NULL) {
		mixer = sc->sc_rmixer;
	} else {
		mixer = sc->sc_pmixer;
	}
	fmt = &mixer->hwbuf.fmt;
	ai->blocksize = mixer->frames_per_block * fmt->stride * fmt->channels;
	ai->mode = file->mode;
	/* hiwat, lowat are meaningless in current implementation */
	ai->hiwat = 0;
	ai->lowat = 0;

	if (need_mixerinfo) {
		p->port = au_get_port(sc, &sc->sc_outports);
		r->port = au_get_port(sc, &sc->sc_inports);

		p->avail_ports = sc->sc_outports.allports;
		r->avail_ports = sc->sc_inports.allports;

		if (ptrack) {
			au_get_gain(sc, &sc->sc_outports, &gain, &p->balance);
			p->gain = ptrack->volume;
		}
		if (rtrack) {
			au_get_gain(sc, &sc->sc_inports, &gain, &r->balance);
			r->gain = ptrack->volume;
		}

		if (sc->sc_monitor_port != -1) {
			gain = au_get_monitor_gain(sc);
			if (gain != -1)
				ai->monitor_gain = gain;
		}
	}

	return 0;
}

static int
audio_getenc(struct audio_softc *sc, struct audio_encoding *ae)
{
	if (ae->index < 0)
		return EINVAL;
	if (ae->index >= sc->sc_encodings_count)
		return EINVAL;

	memcpy(ae, &sc->sc_encodings[ae->index], sizeof(*ae));
	return 0;
}

static int
audio_get_props(struct audio_softc *sc)
{
	const struct audio_hw_if *hw;
	int props;

	KASSERT(mutex_owned(sc->sc_lock));

	hw = sc->hw_if;
	props = hw->get_props(sc->hw_hdl);

	/*
	 * if neither playback nor capture properties are reported,
	 * assume both are supported by the device driver
	 */
	if ((props & (AUDIO_PROP_PLAYBACK|AUDIO_PROP_CAPTURE)) == 0)
		props |= (AUDIO_PROP_PLAYBACK | AUDIO_PROP_CAPTURE);

	props |= AUDIO_PROP_MMAP;

	return props;
}

static bool
audio_can_playback(struct audio_softc *sc)
{
	return sc->sc_can_playback;
}

static bool
audio_can_capture(struct audio_softc *sc)
{
	return sc->sc_can_capture;
}

#ifdef AUDIO_PM_IDLE
static void
audio_idle(void *arg)
{
	device_t dv = arg;
	struct audio_softc *sc = device_private(dv);

#ifdef PNP_DEBUG
	extern int pnp_debug_idle;
	if (pnp_debug_idle)
		printf("%s: idle handler called\n", device_xname(dv));
#endif

	sc->sc_idle = true;

	/* XXX joerg Make pmf_device_suspend handle children? */
	if (!pmf_device_suspend(dv, PMF_Q_SELF))
		return;

	if (!pmf_device_suspend(sc->sc_dev, PMF_Q_SELF))
		pmf_device_resume(dv, PMF_Q_SELF);
}

static void
audio_activity(device_t dv, devactive_t type)
{
	struct audio_softc *sc = device_private(dv);

	if (type != DVA_SYSTEM)
		return;

	callout_schedule(&sc->sc_idle_counter, audio_idle_timeout * hz);

	sc->sc_idle = false;
	if (!device_is_active(dv)) {
		/* XXX joerg How to deal with a failing resume... */
		pmf_device_resume(sc->sc_dev, PMF_Q_SELF);
		pmf_device_resume(dv, PMF_Q_SELF);
	}
}
#endif

static bool
audio_suspend(device_t dv, const pmf_qual_t *qual)
{
	struct audio_softc *sc = device_private(dv);

	mutex_enter(sc->sc_lock);
	audio_mixer_capture(sc);
	// XXX mixer をとめる?
	mutex_enter(sc->sc_intr_lock);
	if (sc->sc_pmixer->busy)
		sc->hw_if->halt_output(sc->hw_hdl);
	if (sc->sc_rmixer->busy)
		sc->hw_if->halt_input(sc->hw_hdl);
	mutex_exit(sc->sc_intr_lock);
#ifdef AUDIO_PM_IDLE
	callout_halt(&sc->sc_idle_counter, sc->sc_lock);
#endif
	mutex_exit(sc->sc_lock);

	return true;
}

static bool
audio_resume(device_t dv, const pmf_qual_t *qual)
{
	struct audio_softc *sc = device_private(dv);
	struct audio_info ai;

	mutex_enter(sc->sc_lock);
#if 0 // XXX ?
	sc->sc_trigger_started = false;
	sc->sc_rec_started = false;

#endif

	audio_mixer_restore(sc);
	/* XXX ? */
	AUDIO_INITINFO(&ai);
	audio_setinfo_hw(sc, &ai);
#if 0	// XXX
	// 再生トラックがあれば再生再開
	// 録音トラックがあれば録音再開
	if (sc->sc_pmixer.available)
		audio_startp(sc);
	if (sc->sc_rmixer.available)
		audio_startr(sc);
#endif
	mutex_exit(sc->sc_lock);

	return true;
}

/*
 * Mixer driver
 */
int
mixer_open(dev_t dev, struct audio_softc *sc, int flags,
    int ifmt, struct lwp *l, struct file **nfp)
{
	struct file *fp;
	audio_file_t *af;
	int error, fd;

	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->hw_if == NULL)
		return  ENXIO;

	DPRINTF(("mixer_open: flags=0x%x sc=%p\n", flags, sc));

	error = fd_allocfile(&fp, &fd);
	if (error)
		return error;

	af = kmem_zalloc(sizeof(*af), KM_SLEEP);
	af->dev = dev;

	error = fd_clone(fp, fd, flags, &audio_fileops, af);
	KASSERT(error == EMOVEFD);

	*nfp = fp;
	return error;
}

/*
 * Remove a process from those to be signalled on mixer activity.
 */
static void
mixer_remove(struct audio_softc *sc)
{
	struct mixer_asyncs **pm, *m;
	pid_t pid;

	KASSERT(mutex_owned(sc->sc_lock));

	pid = curproc->p_pid;
	for (pm = &sc->sc_async_mixer; *pm; pm = &(*pm)->next) {
		if ((*pm)->pid == pid) {
			m = *pm;
			*pm = m->next;
			kmem_free(m, sizeof(*m));
			return;
		}
	}
}

/*
 * Signal all processes waiting for the mixer.
 */
static void
mixer_signal(struct audio_softc *sc)
{
	struct mixer_asyncs *m;
	proc_t *p;

	for (m = sc->sc_async_mixer; m; m = m->next) {
		mutex_enter(proc_lock);
		if ((p = proc_find(m->pid)) != NULL)
			psignal(p, SIGIO);
		mutex_exit(proc_lock);
	}
}

/*
 * Close a mixer device
 */
/* ARGSUSED */
int
mixer_close(struct audio_softc *sc, int flags, audio_file_t *file)
{

	KASSERT(mutex_owned(sc->sc_lock));
	if (sc->hw_if == NULL)
		return ENXIO;

	DPRINTF(("mixer_close: sc %p\n", sc));
	mixer_remove(sc);

	return 0;
}

int
mixer_ioctl(struct audio_softc *sc, u_long cmd, void *addr, int flag,
	    struct lwp *l)
{
	const struct audio_hw_if *hw;
	struct mixer_asyncs *ma;
	mixer_ctrl_t *mc;
	int error;

	DPRINTF(("mixer_ioctl(%lu,'%c',%lu)\n",
		 IOCPARM_LEN(cmd), (char)IOCGROUP(cmd), cmd&0xff));
	hw = sc->hw_if;
	if (hw == NULL)
		return ENXIO;
	error = EINVAL;

	/* we can return cached values if we are sleeping */
	if (cmd != AUDIO_MIXER_READ)
		device_active(sc->dev, DVA_SYSTEM);

	switch (cmd) {
	case FIOASYNC:
		if (*(int *)addr) {
			ma = kmem_alloc(sizeof(struct mixer_asyncs), KM_SLEEP);
		} else {
			ma = NULL;
		}
		mixer_remove(sc);	/* remove old entry */
		if (ma != NULL) {
			ma->next = sc->sc_async_mixer;
			ma->pid = curproc->p_pid;
			sc->sc_async_mixer = ma;
		}
		error = 0;
		break;

	case AUDIO_GETDEV:
		DPRINTF(("AUDIO_GETDEV\n"));
		error = hw->getdev(sc->hw_hdl, (audio_device_t *)addr);
		break;

	case AUDIO_MIXER_DEVINFO:
		DPRINTF(("AUDIO_MIXER_DEVINFO\n"));
		((mixer_devinfo_t *)addr)->un.v.delta = 0; /* default */
		error = audio_query_devinfo(sc, (mixer_devinfo_t *)addr);
		break;

	case AUDIO_MIXER_READ:
		DPRINTF(("AUDIO_MIXER_READ\n"));
		mc = (mixer_ctrl_t *)addr;

		if (device_is_active(sc->sc_dev))
			error = audio_get_port(sc, mc);
		else if (mc->dev < 0 || mc->dev >= sc->sc_nmixer_states)
			error = ENXIO;
		else {
			int dev = mc->dev;
			memcpy(mc, &sc->sc_mixer_state[dev],
			    sizeof(mixer_ctrl_t));
			error = 0;
		}
		break;

	case AUDIO_MIXER_WRITE:
		DPRINTF(("AUDIO_MIXER_WRITE\n"));
		error = audio_set_port(sc, (mixer_ctrl_t *)addr);
		if (!error && hw->commit_settings)
			error = hw->commit_settings(sc->hw_hdl);
		if (!error)
			mixer_signal(sc);
		break;

	default:
		if (hw->dev_ioctl) {
			error = hw->dev_ioctl(sc->hw_hdl, cmd, addr, flag, l);
		} else
			error = EINVAL;
		break;
	}
	DPRINTF(("mixer_ioctl(%lu,'%c',%lu) result %d\n",
		 IOCPARM_LEN(cmd), (char)IOCGROUP(cmd), cmd&0xff, error));
	return error;
}

int
au_portof(struct audio_softc *sc, char *name, int class)
{
	mixer_devinfo_t mi;

	for (mi.index = 0; audio_query_devinfo(sc, &mi) == 0; mi.index++) {
		if (mi.mixer_class == class && strcmp(mi.label.name, name) == 0)
			return mi.index;
	}
	return -1;
}

void
au_setup_ports(struct audio_softc *sc, struct au_mixer_ports *ports,
	       mixer_devinfo_t *mi, const struct portname *tbl)
{
	int i, j;

	ports->index = mi->index;
	if (mi->type == AUDIO_MIXER_ENUM) {
		ports->isenum = true;
		for(i = 0; tbl[i].name; i++)
		    for(j = 0; j < mi->un.e.num_mem; j++)
			if (strcmp(mi->un.e.member[j].label.name,
						    tbl[i].name) == 0) {
				ports->allports |= tbl[i].mask;
				ports->aumask[ports->nports] = tbl[i].mask;
				ports->misel[ports->nports] =
				    mi->un.e.member[j].ord;
				ports->miport[ports->nports] =
				    au_portof(sc, mi->un.e.member[j].label.name,
				    mi->mixer_class);
				if (ports->mixerout != -1 &&
				    ports->miport[ports->nports] != -1)
					ports->isdual = true;
				++ports->nports;
			}
	} else if (mi->type == AUDIO_MIXER_SET) {
		for(i = 0; tbl[i].name; i++)
		    for(j = 0; j < mi->un.s.num_mem; j++)
			if (strcmp(mi->un.s.member[j].label.name,
						tbl[i].name) == 0) {
				ports->allports |= tbl[i].mask;
				ports->aumask[ports->nports] = tbl[i].mask;
				ports->misel[ports->nports] =
				    mi->un.s.member[j].mask;
				ports->miport[ports->nports] =
				    au_portof(sc, mi->un.s.member[j].label.name,
				    mi->mixer_class);
				++ports->nports;
			}
	}
}

int
au_set_lr_value(struct audio_softc *sc, mixer_ctrl_t *ct, int l, int r)
{

	KASSERT(mutex_owned(sc->sc_lock));

	ct->type = AUDIO_MIXER_VALUE;
	ct->un.value.num_channels = 2;
	ct->un.value.level[AUDIO_MIXER_LEVEL_LEFT] = l;
	ct->un.value.level[AUDIO_MIXER_LEVEL_RIGHT] = r;
	if (audio_set_port(sc, ct) == 0)
		return 0;
	ct->un.value.num_channels = 1;
	ct->un.value.level[AUDIO_MIXER_LEVEL_MONO] = (l+r)/2;
	return audio_set_port(sc, ct);
}

int
au_get_lr_value(struct audio_softc *sc, mixer_ctrl_t *ct, int *l, int *r)
{
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	ct->un.value.num_channels = 2;
	if (audio_get_port(sc, ct) == 0) {
		*l = ct->un.value.level[AUDIO_MIXER_LEVEL_LEFT];
		*r = ct->un.value.level[AUDIO_MIXER_LEVEL_RIGHT];
	} else {
		ct->un.value.num_channels = 1;
		error = audio_get_port(sc, ct);
		if (error)
			return error;
		*r = *l = ct->un.value.level[AUDIO_MIXER_LEVEL_MONO];
	}
	return 0;
}

int
au_set_gain(struct audio_softc *sc, struct au_mixer_ports *ports,
	    int gain, int balance)
{
	mixer_ctrl_t ct;
	int i, error;
	int l, r;
	u_int mask;
	int nset;

	KASSERT(mutex_owned(sc->sc_lock));

	if (balance == AUDIO_MID_BALANCE) {
		l = r = gain;
	} else if (balance < AUDIO_MID_BALANCE) {
		l = gain;
		r = (balance * gain) / AUDIO_MID_BALANCE;
	} else {
		r = gain;
		l = ((AUDIO_RIGHT_BALANCE - balance) * gain)
		    / AUDIO_MID_BALANCE;
	}
	DPRINTF(("au_set_gain: gain=%d balance=%d, l=%d r=%d\n",
		 gain, balance, l, r));

	if (ports->index == -1) {
	usemaster:
		if (ports->master == -1)
			return 0; /* just ignore it silently */
		ct.dev = ports->master;
		error = au_set_lr_value(sc, &ct, l, r);
	} else {
		ct.dev = ports->index;
		if (ports->isenum) {
			ct.type = AUDIO_MIXER_ENUM;
			error = audio_get_port(sc, &ct);
			if (error)
				return error;
			if (ports->isdual) {
				if (ports->cur_port == -1)
					ct.dev = ports->master;
				else
					ct.dev = ports->miport[ports->cur_port];
				error = au_set_lr_value(sc, &ct, l, r);
			} else {
				for(i = 0; i < ports->nports; i++)
				    if (ports->misel[i] == ct.un.ord) {
					    ct.dev = ports->miport[i];
					    if (ct.dev == -1 ||
						au_set_lr_value(sc, &ct, l, r))
						    goto usemaster;
					    else
						    break;
				    }
			}
		} else {
			ct.type = AUDIO_MIXER_SET;
			error = audio_get_port(sc, &ct);
			if (error)
				return error;
			mask = ct.un.mask;
			nset = 0;
			for(i = 0; i < ports->nports; i++) {
				if (ports->misel[i] & mask) {
				    ct.dev = ports->miport[i];
				    if (ct.dev != -1 &&
					au_set_lr_value(sc, &ct, l, r) == 0)
					    nset++;
				}
			}
			if (nset == 0)
				goto usemaster;
		}
	}
	if (!error)
		mixer_signal(sc);
	return error;
}

void
au_get_gain(struct audio_softc *sc, struct au_mixer_ports *ports,
	    u_int *pgain, u_char *pbalance)
{
	mixer_ctrl_t ct;
	int i, l, r, n;
	int lgain, rgain;

	KASSERT(mutex_owned(sc->sc_lock));

	lgain = AUDIO_MAX_GAIN / 2;
	rgain = AUDIO_MAX_GAIN / 2;
	if (ports->index == -1) {
	usemaster:
		if (ports->master == -1)
			goto bad;
		ct.dev = ports->master;
		ct.type = AUDIO_MIXER_VALUE;
		if (au_get_lr_value(sc, &ct, &lgain, &rgain))
			goto bad;
	} else {
		ct.dev = ports->index;
		if (ports->isenum) {
			ct.type = AUDIO_MIXER_ENUM;
			if (audio_get_port(sc, &ct))
				goto bad;
			ct.type = AUDIO_MIXER_VALUE;
			if (ports->isdual) {
				if (ports->cur_port == -1)
					ct.dev = ports->master;
				else
					ct.dev = ports->miport[ports->cur_port];
				au_get_lr_value(sc, &ct, &lgain, &rgain);
			} else {
				for(i = 0; i < ports->nports; i++)
				    if (ports->misel[i] == ct.un.ord) {
					    ct.dev = ports->miport[i];
					    if (ct.dev == -1 ||
						au_get_lr_value(sc, &ct,
								&lgain, &rgain))
						    goto usemaster;
					    else
						    break;
				    }
			}
		} else {
			ct.type = AUDIO_MIXER_SET;
			if (audio_get_port(sc, &ct))
				goto bad;
			ct.type = AUDIO_MIXER_VALUE;
			lgain = rgain = n = 0;
			for(i = 0; i < ports->nports; i++) {
				if (ports->misel[i] & ct.un.mask) {
					ct.dev = ports->miport[i];
					if (ct.dev == -1 ||
					    au_get_lr_value(sc, &ct, &l, &r))
						goto usemaster;
					else {
						lgain += l;
						rgain += r;
						n++;
					}
				}
			}
			if (n != 0) {
				lgain /= n;
				rgain /= n;
			}
		}
	}
bad:
	if (lgain == rgain) {	/* handles lgain==rgain==0 */
		*pgain = lgain;
		*pbalance = AUDIO_MID_BALANCE;
	} else if (lgain < rgain) {
		*pgain = rgain;
		/* balance should be > AUDIO_MID_BALANCE */
		*pbalance = AUDIO_RIGHT_BALANCE -
			(AUDIO_MID_BALANCE * lgain) / rgain;
	} else /* lgain > rgain */ {
		*pgain = lgain;
		/* balance should be < AUDIO_MID_BALANCE */
		*pbalance = (AUDIO_MID_BALANCE * rgain) / lgain;
	}
}

int
au_set_port(struct audio_softc *sc, struct au_mixer_ports *ports, u_int port)
{
	mixer_ctrl_t ct;
	int i, error, use_mixerout;

	KASSERT(mutex_owned(sc->sc_lock));

	use_mixerout = 1;
	if (port == 0) {
		if (ports->allports == 0)
			return 0;		/* Allow this special case. */
		else if (ports->isdual) {
			if (ports->cur_port == -1) {
				return 0;
			} else {
				port = ports->aumask[ports->cur_port];
				ports->cur_port = -1;
				use_mixerout = 0;
			}
		}
	}
	if (ports->index == -1)
		return EINVAL;
	ct.dev = ports->index;
	if (ports->isenum) {
		if (port & (port-1))
			return EINVAL; /* Only one port allowed */
		ct.type = AUDIO_MIXER_ENUM;
		error = EINVAL;
		for(i = 0; i < ports->nports; i++)
			if (ports->aumask[i] == port) {
				if (ports->isdual && use_mixerout) {
					ct.un.ord = ports->mixerout;
					ports->cur_port = i;
				} else {
					ct.un.ord = ports->misel[i];
				}
				error = audio_set_port(sc, &ct);
				break;
			}
	} else {
		ct.type = AUDIO_MIXER_SET;
		ct.un.mask = 0;
		for(i = 0; i < ports->nports; i++)
			if (ports->aumask[i] & port)
				ct.un.mask |= ports->misel[i];
		if (port != 0 && ct.un.mask == 0)
			error = EINVAL;
		else
			error = audio_set_port(sc, &ct);
	}
	if (!error)
		mixer_signal(sc);
	return error;
}

int
au_get_port(struct audio_softc *sc, struct au_mixer_ports *ports)
{
	mixer_ctrl_t ct;
	int i, aumask;

	KASSERT(mutex_owned(sc->sc_lock));

	if (ports->index == -1)
		return 0;
	ct.dev = ports->index;
	ct.type = ports->isenum ? AUDIO_MIXER_ENUM : AUDIO_MIXER_SET;
	if (audio_get_port(sc, &ct))
		return 0;
	aumask = 0;
	if (ports->isenum) {
		if (ports->isdual && ports->cur_port != -1) {
			if (ports->mixerout == ct.un.ord)
				aumask = ports->aumask[ports->cur_port];
			else
				ports->cur_port = -1;
		}
		if (aumask == 0)
			for(i = 0; i < ports->nports; i++)
				if (ports->misel[i] == ct.un.ord)
					aumask = ports->aumask[i];
	} else {
		for(i = 0; i < ports->nports; i++)
			if (ct.un.mask & ports->misel[i])
				aumask |= ports->aumask[i];
	}
	return aumask;
}

/*
 * must be called only if sc->sc_monitor_port != -1.
 * return 0 if success, otherwise errno.
 */
static int
au_set_monitor_gain(struct audio_softc *sc, int monitor_gain)
{
	mixer_ctrl_t ct;

	ct.dev = sc->sc_monitor_port;
	ct.type = AUDIO_MIXER_VALUE;
	ct.un.value.num_channels = 1;
	ct.un.value.level[AUDIO_MIXER_LEVEL_MONO] = monitor_gain;
	return audio_set_port(sc, &ct);
}

/*
 * must be called only if sc->sc_monitor_port != -1.
 * return monitor gain if success, otherwise -1.
 */
static int
au_get_monitor_gain(struct audio_softc *sc)
{
	mixer_ctrl_t ct;

	ct.dev = sc->sc_monitor_port;
	ct.type = AUDIO_MIXER_VALUE;
	ct.un.value.num_channels = 1;
	if (audio_get_port(sc, &ct))
		return -1;
	return ct.un.value.level[AUDIO_MIXER_LEVEL_MONO];
}

static int
audio_set_port(struct audio_softc *sc, mixer_ctrl_t *mc)
{
	KASSERT(mutex_owned(sc->sc_lock));

	return sc->hw_if->set_port(sc->hw_hdl, mc);
}

static int
audio_get_port(struct audio_softc *sc, mixer_ctrl_t *mc)
{
	return sc->hw_if->get_port(sc->hw_hdl, mc);
}

static void
audio_mixer_capture(struct audio_softc *sc)
{
	mixer_devinfo_t mi;
	mixer_ctrl_t *mc;

	KASSERT(mutex_owned(sc->sc_lock));

	for (mi.index = 0;; mi.index++) {
		if (audio_query_devinfo(sc, &mi) != 0)
			break;
		KASSERT(mi.index < sc->sc_nmixer_states);
		if (mi.type == AUDIO_MIXER_CLASS)
			continue;
		mc = &sc->sc_mixer_state[mi.index];
		mc->dev = mi.index;
		mc->type = mi.type;
		mc->un.value.num_channels = mi.un.v.num_channels;
		(void)audio_get_port(sc, mc);
	}

	return;
}

static void
audio_mixer_restore(struct audio_softc *sc)
{
	mixer_devinfo_t mi;
	mixer_ctrl_t *mc;

	KASSERT(mutex_owned(sc->sc_lock));

	for (mi.index = 0; ; mi.index++) {
		if (audio_query_devinfo(sc, &mi) != 0)
			break;
		if (mi.type == AUDIO_MIXER_CLASS)
			continue;
		mc = &sc->sc_mixer_state[mi.index];
		(void)audio_set_port(sc, mc);
	}
	if (sc->hw_if->commit_settings)
		sc->hw_if->commit_settings(sc->hw_hdl);

	return;
}

static void
audio_volume_down(device_t dv)
{
	struct audio_softc *sc = device_private(dv);
	mixer_devinfo_t mi;
	int newgain;
	u_int gain;
	u_char balance;

	mutex_enter(sc->sc_lock);
	if (sc->sc_outports.index == -1 && sc->sc_outports.master != -1) {
		mi.index = sc->sc_outports.master;
		mi.un.v.delta = 0;
		if (audio_query_devinfo(sc, &mi) == 0) {
			au_get_gain(sc, &sc->sc_outports, &gain, &balance);
			newgain = gain - mi.un.v.delta;
			if (newgain < AUDIO_MIN_GAIN)
				newgain = AUDIO_MIN_GAIN;
			au_set_gain(sc, &sc->sc_outports, newgain, balance);
		}
	}
	mutex_exit(sc->sc_lock);
}

static void
audio_volume_up(device_t dv)
{
	struct audio_softc *sc = device_private(dv);
	mixer_devinfo_t mi;
	u_int gain, newgain;
	u_char balance;

	mutex_enter(sc->sc_lock);
	if (sc->sc_outports.index == -1 && sc->sc_outports.master != -1) {
		mi.index = sc->sc_outports.master;
		mi.un.v.delta = 0;
		if (audio_query_devinfo(sc, &mi) == 0) {
			au_get_gain(sc, &sc->sc_outports, &gain, &balance);
			newgain = gain + mi.un.v.delta;
			if (newgain > AUDIO_MAX_GAIN)
				newgain = AUDIO_MAX_GAIN;
			au_set_gain(sc, &sc->sc_outports, newgain, balance);
		}
	}
	mutex_exit(sc->sc_lock);
}

static void
audio_volume_toggle(device_t dv)
{
	struct audio_softc *sc = device_private(dv);
	u_int gain, newgain;
	u_char balance;

	mutex_enter(sc->sc_lock);
	au_get_gain(sc, &sc->sc_outports, &gain, &balance);
	if (gain != 0) {
		sc->sc_lastgain = gain;
		newgain = 0;
	} else
		newgain = sc->sc_lastgain;
	au_set_gain(sc, &sc->sc_outports, newgain, balance);
	mutex_exit(sc->sc_lock);
}

static int
audio_query_devinfo(struct audio_softc *sc, mixer_devinfo_t *di)
{
	char mixLabel[255];

	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->sc_static_nmixer_states == 0 || sc->sc_nmixer_states == 0)
		goto hardware;

	if (di->index >= sc->sc_static_nmixer_states - 1 &&
	    di->index < sc->sc_nmixer_states) {
		if (di->index == sc->sc_static_nmixer_states - 1) {
			di->mixer_class = sc->sc_static_nmixer_states -1;
			di->next = di->prev = AUDIO_MIXER_LAST;
			strcpy(di->label.name, AudioCvirtchan);
			di->type = AUDIO_MIXER_CLASS;
		} else if ((di->index - sc->sc_static_nmixer_states) % 2 == 0) {
			di->mixer_class = sc->sc_static_nmixer_states -1;
			snprintf(mixLabel, sizeof(mixLabel), AudioNdac"%d",
			    (di->index - sc->sc_static_nmixer_states) / 2);
			strcpy(di->label.name, mixLabel);
			di->type = AUDIO_MIXER_VALUE;
			di->next = di->prev = AUDIO_MIXER_LAST;
			di->un.v.num_channels = 1;
			strcpy(di->un.v.units.name, AudioNvolume);
		} else {
			di->mixer_class = sc->sc_static_nmixer_states -1;
			snprintf(mixLabel, sizeof(mixLabel),
			    AudioNmicrophone "%d",
			    (di->index - sc->sc_static_nmixer_states) / 2);
			strcpy(di->label.name, mixLabel);
			di->type = AUDIO_MIXER_VALUE;
			di->next = di->prev = AUDIO_MIXER_LAST;
			di->un.v.num_channels = 1;
			strcpy(di->un.v.units.name, AudioNvolume);
		}

		return 0;
	}

hardware:
	return sc->hw_if->query_devinfo(sc->hw_hdl, di);
}

// さすがに見直したほうがよさそう
// grow_mixer_states()
// shrink_mixer_states()

#endif /* NAUDIO > 0 */

#if NAUDIO == 0 && (NMIDI > 0 || NMIDIBUS > 0)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/audioio.h>
#include <dev/audio_if.h>
#endif

#if NAUDIO > 0 || (NMIDI > 0 || NMIDIBUS > 0)
int
audioprint(void *aux, const char *pnp)
{
	struct audio_attach_args *arg;
	const char *type;

	if (pnp != NULL) {
		arg = aux;
		switch (arg->type) {
		case AUDIODEV_TYPE_AUDIO:
			type = "audio";
			break;
		case AUDIODEV_TYPE_MIDI:
			type = "midi";
			break;
		case AUDIODEV_TYPE_OPL:
			type = "opl";
			break;
		case AUDIODEV_TYPE_MPU:
			type = "mpu";
			break;
		default:
			panic("audioprint: unknown type %d", arg->type);
		}
		aprint_normal("%s at %s", type, pnp);
	}
	return UNCONF;
}

#endif /* NAUDIO > 0 || (NMIDI > 0 || NMIDIBUS > 0) */

#ifdef _MODULE

extern struct cfdriver audio_cd;
devmajor_t audio_bmajor = -1, audio_cmajor = -1;

#include "ioconf.c"

#endif

MODULE(MODULE_CLASS_DRIVER, audio, NULL);

static int
audio_modcmd(modcmd_t cmd, void *arg)
{
	int error = 0;

#ifdef _MODULE
	switch (cmd) {
	case MODULE_CMD_INIT:
		error = devsw_attach(audio_cd.cd_name, NULL, &audio_bmajor,
		    &audio_cdevsw, &audio_cmajor);
		if (error)
			break;

		error = config_init_component(cfdriver_ioconf_audio,
		    cfattach_ioconf_audio, cfdata_ioconf_audio);
		if (error) {
			devsw_detach(NULL, &audio_cdevsw);
		}
		break;
	case MODULE_CMD_FINI:
		devsw_detach(NULL, &audio_cdevsw);
		error = config_fini_component(cfdriver_ioconf_audio,
		   cfattach_ioconf_audio, cfdata_ioconf_audio);
		if (error)
			devsw_attach(audio_cd.cd_name, NULL, &audio_bmajor,
			    &audio_cdevsw, &audio_cmajor);
		break;
	default:
		error = ENOTTY;
		break;
	}
#endif

	return error;
}

#include <dev/audio/aumix.c>
#include <dev/audio/aucodec.c>
#include <dev/audio/aucodec_linear.c>
#include <dev/audio/aucodec_mulaw.c>


// x audioctl play.gain と mixerctl outputs.master は N7 では連動していたし
//   ドキュメントにもそう書いてある
// x 全般的に AUDIO_SETINFO どうすんだというのはある
//  - pause と他の設定変更を同時にするのはやめたほうがいい
//  - 全体的にロールバックできてなさそう
// x audio(4) に AUDIO_PERROR がない (RERROR はある)
// x AUDIO_RERROR はサンプル数と読めるが、実際に返すのはバイト数。
// x lastinfo を全トラックが持っているが、ハードウェア部分は1つの lastinfo
//   に分離しないと、suspend/resume の時におかしくなっている。
// x SETINFO の gain はソフトゲインだが GETINFO はハードウェアのほうを
//   取得しているっぽい
// x gain の範囲チェックは誰がしてるんだ
// x suspend 中に uaudio が消えるケースはまた後で
// x audio_clear がどうして非対称なのか
// x audio_clear はたぶんハード/ソフトレイヤを分離する必要がある
// o monitor_gain の get/set は commit してもいいかも
// x SETFD (hw_if->setfd) はバグってるけど誰も使ってないので、
//   消して、hw->open が full-duplex にセットする、でいいんじゃないかな。
// x pad(4) がリソースリークしてる
// x sample_rate 0 で write すると死ぬ
// x audioplay(1) も sample_rate 0 のチェックを一切してないっぽい。
// x 今の実装だと attach 時の setinfo で speaker_out が動いてしまう。
//   どうせその後の open で正しい状態になるから構わないけど、
//   attach 時点で呼び出す必要はなさげ。
// x sysctl で channels を設定できない値にした後、設定可能な値にすると
//   前の値が見える。
//   sysctl -w hw.uaudio0.channels=9
//   invalid argument
//   sysctl -w hw.uaudio0.channels=2
//   9->2
