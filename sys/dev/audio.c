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
#include <dev/audiovar.h>
#include <dev/auconv.h>

#include <machine/endian.h>

#include <uvm/uvm.h>

#include "ioconf.h"

#ifdef AUDIO_DEBUG
#define DPRINTF(n, fmt...)	do {	\
	if (audiodebug >= (n))		\
		printf(fmt);		\
} while (0)
int	audiodebug = AUDIO_DEBUG;
#else
#define DPRINTF(n, fmt...)	do { } while (0)
#endif

#define SPECIFIED(x)	((x) != ~0)
#define SPECIFIED_CH(x)	((x) != (u_char)~0)

/* #define AUDIO_PM_IDLE */
#ifdef AUDIO_PM_IDLE
int	audio_idle_timeout = 30;
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
static int audio_waitio(struct audio_softc *, audio_track_t *);

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
//static int audio_read(struct audio_softc *, struct uio *, int, audio_file_t *);
//static int audio_write(struct audio_softc *, struct uio *, int, audio_file_t *);
static void audio_file_clear(struct audio_softc *, audio_file_t *);
static void audio_hw_clear(struct audio_softc *);
static int audio_ioctl(dev_t, struct audio_softc *, u_long, void *, int,
		       struct lwp *, audio_file_t *);
static int audio_poll(struct audio_softc *, int, struct lwp *, audio_file_t *);
static int audio_kqfilter(struct audio_softc *, audio_file_t *, struct knote *);
static int audio_mmap(struct audio_softc *, off_t *, size_t, int, int *, int *,
	struct uvm_object **, int *, audio_file_t *);

static int audiostartr(struct audio_softc *);
static int audiostartp(struct audio_softc *);
//static void audio_pintr(void *);
//static void audio_rintr(void *);

static int audio_query_devinfo(struct audio_softc *, mixer_devinfo_t *);

static int audio_file_setinfo(struct audio_softc *, audio_file_t *,
	const struct audio_info *);
static int audio_file_setinfo_check(audio_format2_t *,
	const struct audio_prinfo *);
static int audio_file_setinfo_set(audio_track_t *, const struct audio_prinfo *,
	bool, audio_format2_t *, int);
static int audio_setinfo_hw(struct audio_softc *, struct audio_info *);
static int audio_set_params(struct audio_softc *, int);
static int audiogetinfo(struct audio_softc *, struct audio_info *, int,
	audio_file_t *);
static int audio_getenc(struct audio_softc *, struct audio_encoding *);
static int audio_get_props(struct audio_softc *);
static bool audio_can_playback(struct audio_softc *);
static bool audio_can_capture(struct audio_softc *);
static int audio_check_params(struct audio_params *);
static int audio_check_params2(audio_format2_t *);
static int audio_select_freq(const struct audio_format *);
static int audio_hw_config_fmt(struct audio_softc *, audio_format2_t *, int);
static int audio_hw_config_by_format(struct audio_softc *, audio_format2_t *,
	int);
static int audio_hw_config_by_encoding(struct audio_softc *, audio_format2_t *,
	int);
static int audio_hw_config(struct audio_softc *, int);
static int audio_sysctl_volume(SYSCTLFN_PROTO);
static void audio_format2_tostr(char *, size_t, const audio_format2_t *);
#ifdef AUDIO_DEBUG
static void audio_print_format2(const char *, const audio_format2_t *);
#endif

#ifdef OLD_FILTER
static void stream_filter_list_append(stream_filter_list_t *,
		stream_filter_factory_t, const audio_params_t *);
static void stream_filter_list_prepend(stream_filter_list_t *,
		stream_filter_factory_t, const audio_params_t *);
static void stream_filter_list_set(stream_filter_list_t *, int,
		stream_filter_factory_t, const audio_params_t *);
#endif

// この track が再生トラックなら true を返します。
static inline bool
audio_track_is_playback(const audio_track_t *track)
{
	return ((track->mode & AUMODE_PLAY) != 0);
}

// この track が録音トラックなら true を返します。
static inline bool
audio_track_is_record(const audio_track_t *track)
{
	return ((track->mode & AUMODE_RECORD) != 0);
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
	.fo_name = "audio",
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

static const char *encoding_names[] = {
	"none",
	AudioEmulaw,
	AudioEalaw,
	"pcm16",
	"pcm8",
	AudioEadpcm,
	AudioEslinear_le,
	AudioEslinear_be,
	AudioEulinear_le,
	AudioEulinear_be,
	AudioEslinear,
	AudioEulinear,
	AudioEmpeg_l1_stream,
	AudioEmpeg_l1_packets,
	AudioEmpeg_l1_system
	AudioEmpeg_l2_stream,
	AudioEmpeg_l2_packets,
	AudioEmpeg_l2_system,
	AudioEac3,
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

#if 1
static char *audio_buildinfo;

// ビルドオプションを文字列にします。(開発用)
// テスト用なので解放してません。
static void
make_buildinfo(void)
{
	char buf[100];
	int n;

	n = 0;
	n += snprintf(buf, sizeof(buf), "AUDIO_BLK_MS=%d", AUDIO_BLK_MS);
	n += snprintf(buf + n, sizeof(buf) - n, ", NBLKOUT=%d", NBLKOUT);
#if defined(FREQ_ORIG)
	n += snprintf(buf + n, sizeof(buf) - n, ", FREQ_ORIG");
#endif
#if defined(FREQ_CYCLE2)
	n += snprintf(buf + n, sizeof(buf) - n, ", FREQ_CYCLE2");
#endif
#if defined(START_ON_OPEN)
	n += snprintf(buf + n, sizeof(buf) - n, ", START_ON_OPEN");
#endif
#if defined(USE_SETCHAN)
	n += snprintf(buf + n, sizeof(buf) - n, ", USE_SETCHAN");
#endif

	audio_buildinfo = malloc(strlen(buf) + 1, M_NOWAIT, 0);
	if (audio_buildinfo) {
		strcpy(audio_buildinfo, buf);
	}
}
#endif

static int
audiomatch(device_t parent, cfdata_t match, void *aux)
{
	struct audio_attach_args *sa;

	sa = aux;
	DPRINTF(1, "%s: type=%d sa=%p hw=%p\n",
	     __func__, sa->type, sa, sa->hwif);
	return (sa->type == AUDIODEV_TYPE_AUDIO) ? 1 : 0;
}

static void
audioattach(device_t parent, device_t self, void *aux)
{
	struct audio_softc *sc;
	struct audio_attach_args *sa;
	const struct audio_hw_if *hw_if;
	const struct sysctlnode *node;
	char fmtstr[64];
	void *hdlp;
	bool is_indep;
	int props;
	int blkms;
	int error;

	sc = device_private(self);
	sc->dev = self;
	sa = (struct audio_attach_args *)aux;
	hw_if = sa->hwif;
	hdlp = sa->hdl;

	if (hw_if == NULL || hw_if->get_locks == NULL) {
		panic("audioattach: missing hw_if method");
	}

	hw_if->get_locks(hdlp, &sc->sc_intr_lock, &sc->sc_lock);

#ifdef DIAGNOSTIC
	if (hw_if->query_encoding == NULL ||
	    (hw_if->set_params == NULL && hw_if->set_params2 == NULL) ||
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
	error = audio_hw_config(sc, is_indep);

	/* init track mixer */
	if (sc->sc_can_playback) {
		sc->sc_pmixer = kmem_zalloc(sizeof(*sc->sc_pmixer),
		    KM_SLEEP);
		error = audio_mixer_init(sc, sc->sc_pmixer, AUMODE_PLAY);
		if (error == 0) {
			audio_format2_tostr(fmtstr, sizeof(fmtstr),
			    &sc->sc_pmixer->hwbuf.fmt);
			blkms = sc->sc_pmixer->blktime_n * 1000 /
			    sc->sc_pmixer->blktime_d;
			aprint_normal_dev(sc->dev,
			    "%s, blk %dms for playback\n",
			    fmtstr, blkms);
		} else {
			aprint_error_dev(sc->dev,
			    "configuring playback mode failed\n");
			kmem_free(sc->sc_pmixer, sizeof(*sc->sc_pmixer));
			sc->sc_pmixer = NULL;
			sc->sc_can_playback = false;
		}
	}
	if (sc->sc_can_capture) {
		sc->sc_rmixer = kmem_zalloc(sizeof(*sc->sc_rmixer),
		    KM_SLEEP);
		error = audio_mixer_init(sc, sc->sc_rmixer, AUMODE_RECORD);
		if (error == 0) {
			audio_format2_tostr(fmtstr, sizeof(fmtstr),
			    &sc->sc_rmixer->hwbuf.fmt);
			blkms = sc->sc_rmixer->blktime_n * 1000 /
			    sc->sc_rmixer->blktime_d;
			aprint_normal_dev(sc->dev,
			    "%s, blk %dms for recording\n",
			    fmtstr, blkms);
		} else {
			aprint_error_dev(sc->dev,
			    "configuring record mode failed\n");
			kmem_free(sc->sc_rmixer, sizeof(*sc->sc_rmixer));
			sc->sc_rmixer = NULL;
			sc->sc_can_capture = false;
		}
	}

	if (sc->sc_can_playback == false && sc->sc_can_capture == false)
		goto bad;

	// setfd は誰一人実装してないし廃止したい方向

	sc->sc_sih_rd = softint_establish(SOFTINT_SERIAL | SOFTINT_MPSAFE,
	    audio_softintr_rd, sc);

	// /dev/sound のデフォルト値
	sc->sc_pparams = params_to_format2(&audio_default);
	sc->sc_rparams = params_to_format2(&audio_default);
	sc->sc_ppause = false;
	sc->sc_rpause = false;

	// XXX sc_ai の初期化について考えないといけない
	// 今は sc_ai が一度でも初期化されたかどうかフラグがあるが
	// できればなくしたい。

	mixer_init(sc);
	DPRINTF(2, "audio_attach: inputs ports=0x%x, input master=%d, "
		 "output ports=0x%x, output master=%d\n",
		 sc->sc_inports.allports, sc->sc_inports.master,
		 sc->sc_outports.allports, sc->sc_outports.master);

	sysctl_createv(&sc->sc_log, 0, NULL, &node,
		0,
		CTLTYPE_NODE, device_xname(sc->dev),
		SYSCTL_DESCR("audio test"),
		NULL, 0,
		NULL, 0,
		CTL_HW,
		CTL_CREATE, CTL_EOL);

	if (node != NULL) {
		sysctl_createv(&sc->sc_log, 0, NULL, NULL,
			CTLFLAG_READWRITE,
			CTLTYPE_INT, "volume",
			SYSCTL_DESCR("software volume test"),
			audio_sysctl_volume, 0, (void *)sc, 0,
			CTL_HW, node->sysctl_num, CTL_CREATE, CTL_EOL);

#if 1
		// デバッグ用のビルドオプション表示
		if (audio_buildinfo == NULL)
			make_buildinfo();

		sysctl_createv(&sc->sc_log, 0, NULL, NULL,
			CTLFLAG_PERMANENT,
			CTLTYPE_STRING, "buildinfo",
			SYSCTL_DESCR("audio build options"),
			NULL, 0, audio_buildinfo, 0,
			CTL_HW, node->sysctl_num, CTL_CREATE, CTL_EOL);
#endif
	}

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
	return;

bad:
	// アタッチがエラーを返せない構造なので、ここでエラーになっても
	// デバイス自体は出来てしまう。そこで hw_if == NULL なら
	// configure されてないものとする。という運用。コメント書けよ。
	sc->hw_if = NULL;
	aprint_error_dev(sc->dev, "disabled\n");
	return;
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
	audio_file_t *f;
	int maj, mn;
	//int rc;

	sc = device_private(self);
	DPRINTF(1, "%s: sc=%p flags=%d\n", __func__, sc, flags);

	/* Start draining existing accessors of the device. */
	// なぜここで config_detach_children() ?

	// 稼働中のトラックを終了させる
	mutex_enter(sc->sc_lock);
	sc->sc_dying = true;
	SLIST_FOREACH(f, &sc->sc_files, entry) {
		if (f->ptrack)
			cv_broadcast(&f->ptrack->outchan);
		if (f->rtrack)
			cv_broadcast(&f->rtrack->outchan);
	}
	cv_broadcast(&sc->sc_pmixer->draincv);
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
	if (sc->sc_pmixer) {
		audio_mixer_destroy(sc, sc->sc_pmixer);
		kmem_free(sc->sc_pmixer, sizeof(*sc->sc_pmixer));
	}
	if (sc->sc_rmixer) {
		audio_mixer_destroy(sc, sc->sc_rmixer);
		kmem_free(sc->sc_rmixer, sizeof(*sc->sc_rmixer));
	}

	if (sc->sc_sih_rd) {
		softint_disestablish(sc->sc_sih_rd);
		sc->sc_sih_rd = NULL;
	}

#ifdef AUDIO_PM_IDLE
	callout_destroy(&sc->sc_idle_counter);
#endif
	seldestroy(&sc->sc_rsel);
	seldestroy(&sc->sc_wsel);

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
		error = audio_kqfilter(sc, file, kn);
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
	struct audio_info ai;
	struct file *fp;
	audio_file_t *af;
	int fd;
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->hw_if == NULL)
		return ENXIO;

	DPRINTF(1, "audio_open: flags=0x%x sc=%p hdl=%p\n",
		 flags, sc, sc->hw_hdl);

	af = kmem_zalloc(sizeof(audio_file_t), KM_SLEEP);
	af->sc = sc;
	af->dev = dev;
	if ((flags & FWRITE) != 0 && audio_can_playback(sc))
		af->mode |= AUMODE_PLAY | AUMODE_PLAY_ALL;
	if ((flags & FREAD) != 0 && audio_can_capture(sc))
		af->mode |= AUMODE_RECORD;
	if (af->mode == 0) {
		error = ENXIO;
		goto bad1;
	}

	// トラックの初期化
	// トラックバッファの初期化
	if ((af->mode & AUMODE_PLAY) != 0) {
		error = audio_track_init(sc, &af->ptrack, AUMODE_PLAY);
		if (error)
			goto bad1;
	}
	if ((af->mode & AUMODE_RECORD) != 0) {
		error = audio_track_init(sc, &af->rtrack, AUMODE_RECORD);
		if (error)
			goto bad2;
	}

	/*
	 * Multiplex device: /dev/audio (MU-Law) and /dev/sound (linear)
	 * The /dev/audio is always (re)set to 8-bit MU-Law mono
	 * For the other devices, you get what they were last set to.
	 */
	AUDIO_INITINFO(&ai);
	if (ISDEVAUDIO(dev)) {
		ai.play.sample_rate   = audio_default.sample_rate;
		ai.play.encoding      = audio_default.encoding;
		ai.play.channels      = audio_default.channels;
		ai.play.precision     = audio_default.precision;
		ai.play.pause         = false;
		ai.record.sample_rate = audio_default.sample_rate;
		ai.record.encoding    = audio_default.encoding;
		ai.record.channels    = audio_default.channels;
		ai.record.precision   = audio_default.precision;
		ai.record.pause       = false;
	}
	ai.mode = af->mode;
	error = audio_file_setinfo(sc, af, &ai);
	if (error)
		goto bad3;

	// 録再合わせて1本目のオープンなら {
	//	kauth の処理?
	//	hw_if->open
	//	下トラックの初期化?
	// } else if (複数ユーザがオープンすることを許可してなければ) {
	//	チェック?
	// }
	if (sc->sc_popens + sc->sc_ropens == 0) {
		/* First open */

		sc->sc_cred = kauth_cred_get();
		kauth_cred_hold(sc->sc_cred);

		if (sc->hw_if->open) {
			mutex_enter(sc->sc_intr_lock);
			error = sc->hw_if->open(sc->hw_hdl, af->mode);
			mutex_exit(sc->sc_intr_lock);
			if (error)
				goto bad3;
		}

		/* Set speaker mode if half duplex */
		// XXX audio(9) にはハーフで録再を切り替える際に使うと
		// 書いてある気がするんだけど、あと
		// Full dup で録再同時に起きてたらどうするのかとか。
		if (!sc->sc_full_duplex || 1/*XXX*/) {
			if (sc->hw_if->speaker_ctl) {
				int on;
				if (af->ptrack) {
					on = 1;
				} else {
					on = 0;
				}
				mutex_enter(sc->sc_intr_lock);
				error = sc->hw_if->speaker_ctl(sc->hw_hdl, on);
				mutex_exit(sc->sc_intr_lock);
				if (error)
					goto bad3;
			}
		}
	} else /* if (sc->sc_multiuser == false) */ {
		uid_t euid = kauth_cred_geteuid(kauth_cred_get());
		if (euid != 0 && kauth_cred_geteuid(sc->sc_cred) != euid) {
			error = EPERM;
			goto bad3;
		}
	}

	// init_input/output
	if (af->ptrack && sc->sc_popens == 0) {
		if (sc->hw_if->init_output) {
			audio_ring_t *hwbuf = &sc->sc_pmixer->hwbuf;
			mutex_enter(sc->sc_intr_lock);
			error = sc->hw_if->init_output(sc->hw_hdl,
			    hwbuf->sample,
			    hwbuf->capacity *
			    hwbuf->fmt.channels * hwbuf->fmt.stride / NBBY);
			mutex_exit(sc->sc_intr_lock);
			if (error)
				goto bad3;
		}
#if defined(START_ON_OPEN)
		audio_pmixer_start(sc->sc_pmixer, false);
#endif
	}
	if (af->rtrack && sc->sc_ropens == 0) {
		if (sc->hw_if->init_input) {
			audio_ring_t *hwbuf = &sc->sc_rmixer->hwbuf;
			mutex_enter(sc->sc_intr_lock);
			error = sc->hw_if->init_input(sc->hw_hdl,
			    hwbuf->sample,
			    hwbuf->capacity *
			    hwbuf->fmt.channels * hwbuf->fmt.stride / NBBY);
			mutex_exit(sc->sc_intr_lock);
			if (error)
				goto bad3;
		}
#if defined(START_ON_OPEN)
		//audio_rmixer_start(sc->sc_rmixer, false);
#endif
	}

	error = fd_allocfile(&fp, &fd);
	if (error)
		goto bad3;

	// このミキサーどうするか
	//grow_mixer_states(sc, 2);

	// オープンカウント++
	if (af->ptrack)
		sc->sc_popens++;
	if (af->rtrack)
		sc->sc_ropens++;
	mutex_enter(sc->sc_intr_lock);
	SLIST_INSERT_HEAD(&sc->sc_files, af, entry);
	mutex_exit(sc->sc_intr_lock);

	error = fd_clone(fp, fd, flags, &audio_fileops, af);
	KASSERT(error == EMOVEFD);

	*nfp = fp;
	TRACEF(af, "done");
	return error;

	// ここの track は sc_files につながっていないので、
	// intr_lock とらずに track_destroy() を呼んでいいはず。
bad3:
	if (af->rtrack) {
		audio_track_destroy(af->rtrack);
		af->rtrack = NULL;
	}
bad2:
	if (af->ptrack) {
		audio_track_destroy(af->ptrack);
		af->ptrack = NULL;
	}
bad1:
	kmem_free(af, sizeof(*af));
	return error;
}

int
audio_drain(struct audio_softc *sc, audio_track_t *track)
{
	return audio_track_drain(track);
}

int
audio_close(struct audio_softc *sc, int flags, audio_file_t *file)
{
	audio_track_t *oldtrack;
	int error;

#if AUDIO_DEBUG > 2
	TRACEF(file, "start po=%d ro=%d", sc->sc_popens, sc->sc_ropens);
#else
	DPRINTF(1, "%s\n", __func__);
#endif
	KASSERT(mutex_owned(sc->sc_lock));

	// いる?
	//if (sc->sc_opens == 0 && sc->sc_recopens == 0)
	//	return ENXIO;

	// 現在どちらのトラックが有効かは file->ptrack、file->rtrack が
	// !NULL で判断すること。
	// flags はユーザが open 時に指示したモードだが、実際には
	// flags よりトラックが少ない場合もある (例えば O_RDWR でオープン
	// しようとして片方のトラックがエラーだとか、HW が再生専用だとか)。
	// そのためここで引数の flags を見てはいけない。

	// SB とかいくつかのドライバは halt_input と halt_output に
	// 同じルーチンを使用しているので、その場合は full duplex なら
	// halt_input を呼ばなくする?。ドライバのほうを直すべき。
	/*
	 * XXX Some drivers (e.g. SB) use the same routine
	 * to halt input and output so don't halt input if
	 * in full duplex mode.  These drivers should be fixed.
	 */
	// XXX これはドライバのほうを先に直せば済む話
	if ((file->ptrack != NULL && file->rtrack != NULL) &&
	    sc->sc_full_duplex == false &&
	    sc->hw_if->halt_input == sc->hw_if->halt_output &&
	    sc->sc_ropens == 1) {
		aprint_error_dev(sc->dev,
		    "%s has halt_input == halt_output. Please fix it\n",
		    device_xname(sc->sc_dev));
		// そうは言いつつもとりあえず回避はしておく
		mutex_enter(sc->sc_intr_lock);
		sc->sc_rbusy = false;
		sc->sc_rmixer->hwbuf.top = 0;
		mutex_exit(sc->sc_intr_lock);
	}

	// これが最後の録音トラックなら、halt_input を呼ぶ?
	if (file->rtrack) {
		if (sc->sc_ropens == 1) {
			if (sc->sc_rbusy) {
				DPRINTF(2, "%s halt_input\n", __func__);
				mutex_enter(sc->sc_intr_lock);
				error = audio2_halt_input(sc);
				mutex_exit(sc->sc_intr_lock);
				if (error) {
					aprint_error_dev(sc->dev,
					    "halt_input failed with %d\n",
					    error);
				}
			}
		}
		oldtrack = file->rtrack;
		mutex_enter(sc->sc_intr_lock);
		file->rtrack = NULL;
		mutex_exit(sc->sc_intr_lock);
		audio_track_destroy(oldtrack);

		KASSERT(sc->sc_ropens > 0);
		sc->sc_ropens--;
	}

	// 再生トラックなら、audio_drain を呼ぶ
	// 最後の再生トラックなら、hw audio_drain、halt_output を呼ぶ
	if (file->ptrack) {
		audio_drain(sc, file->ptrack);

		if (sc->sc_popens == 1) {
			if (sc->sc_pbusy) {
				mutex_enter(sc->sc_intr_lock);
				error = audio2_halt_output(sc);
				mutex_exit(sc->sc_intr_lock);
				if (error) {
					aprint_error_dev(sc->dev,
					    "halt_output failed with %d\n",
					    error);
				}
			}
		}
		oldtrack = file->ptrack;
		mutex_enter(sc->sc_intr_lock);
		file->ptrack = NULL;
		mutex_exit(sc->sc_intr_lock);
		audio_track_destroy(oldtrack);

		KASSERT(sc->sc_popens > 0);
		sc->sc_popens--;
	}

	// 最後なら close
	if (sc->sc_popens + sc->sc_ropens == 0) {
		if (sc->hw_if->close) {
			DPRINTF(2, "%s hw_if close\n", __func__);
			mutex_enter(sc->sc_intr_lock);
			sc->hw_if->close(sc->hw_hdl);
			mutex_exit(sc->sc_intr_lock);
		}

		kauth_cred_free(sc->sc_cred);
	}

	// リストから削除
	mutex_enter(sc->sc_intr_lock);
	SLIST_REMOVE(&sc->sc_files, file, audio_file, entry);
	mutex_exit(sc->sc_intr_lock);

	TRACE("done");
	return 0;
}

// ここに audio_read

// この file の再生 track と録音 track を即座に空にします。
// ミキサーおよび HW には関与しません。呼ばれるのは以下の2か所から:
// o audio_ioctl AUDIO_FLUSH で一切合切クリアする時
// o audiosetinfo でパラメータを変更する必要がある時
//
// 元々 audio_clear() で、録音再生をその場で停止して hw halt_input/output も
// 呼んでいた (呼ぶ必要があったのかどうかは分からない)。
static void
audio_file_clear(struct audio_softc *sc, audio_file_t *file)
{

	if (file->ptrack)
		audio_track_clear(sc, file->ptrack);
	if (file->rtrack)
		audio_track_clear(sc, file->rtrack);
}

// こっちは HW の再生/録音をクリアする??
// 入出力port の切り替えのために必要らしいけど、ほんとか。
void
audio_hw_clear(struct audio_softc *sc)
{
	mutex_enter(sc->sc_intr_lock);
	DPRINTF(1, "%s not implemented\n", __func__);
	mutex_exit(sc->sc_intr_lock);
}

// ここに audio_write

// たぶん audio_ioctl はハードウェアレイヤの変更は行わないはずなので
// これ実行中に同時に他の操作が来ても大丈夫?
// AUDIO_FLUSH .. トラックまでしか触らない。
// AUDIO_DRAIN .. トラックまでしか触らない。
// AUDIO_SETFD .. これもたぶんトラックだけで済ませたほうがいい。
int
audio_ioctl(dev_t dev, struct audio_softc *sc, u_long cmd, void *addr, int flag,
	    struct lwp *l, audio_file_t *file)
{
	//struct audio_offset *ao;
	int error/*, offs*/, fd;

	KASSERT(mutex_owned(sc->sc_lock));

#if defined(USE_SETCHAN)
	// ioctl ターゲットチャンネルを探す
	// XXX デバッグ用
	if (file->ioctl_target != 0) {
		audio_file_t *f;
		audio_file_t *target;

		target = NULL;
		SLIST_FOREACH(f, &sc->sc_files, entry) {
			if (f->ptrack && f->ptrack->id == file->ioctl_target) {
				target = f;
				break;
			}
			if (f->rtrack && f->rtrack->id == file->ioctl_target) {
				target = f;
				break;
			}
		}
		if (target == NULL) {
			DPRINTF(1, "%s: ioctl_target %d not found\n",
			    __func__, file->ioctl_target);
			return EBADF;
		}
		file = target;
	}
#endif /* USE_SETCHAN */

#if defined(AUDIO_DEBUG)
	const char *ioctlnames[] = {
		" AUDIO_GETINFO",	// 21
		" AUDIO_SETINFO",	// 22
		" AUDIO_DRAIN",		// 23
		" AUDIO_FLUSH",		// 24
		" AUDIO_WSEEK",		// 25
		" AUDIO_RERROR",	// 26
		" AUDIO_GETDEV",	// 27
		" AUDIO_GETENC",	// 28
		" AUDIO_GETFD",		// 29
		" AUDIO_SETFD",		// 30
		" AUDIO_PERROR",	// 31
		" AUDIO_GETIOFFS",	// 32
		" AUDIO_GETOOFFS",	// 33
		" AUDIO_GETPROPS",	// 34
		" AUDIO_GETBUFINFO",	// 35
		" AUDIO_SETCHAN",	// 36
		" AUDIO_GETCHAN",	// 37
	};
	int nameidx = (cmd & 0xff);
	const char *ioctlname = "";
	if (21 <= nameidx && nameidx <=37)
		ioctlname = ioctlnames[nameidx - 21];
	DPRINTF(2, "audio_ioctl(%lu,'%c',%lu)%s\n",
		 IOCPARM_LEN(cmd), (char)IOCGROUP(cmd), cmd&0xff, ioctlname);
#endif
	if (sc->hw_if == NULL)
		return ENXIO;
	error = 0;
	switch (cmd) {
#if defined(USE_SETCHAN)
	case AUDIO_SETCHAN:
		file->ioctl_target = *(int *)addr;
		DPRINTF(1, "AUDIO_SETCHAN target id = %d\n",
		    file->ioctl_target);
		break;
#else
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

	case FIONREAD:
		// 入力バッファにあるバイト数
		// XXX 動作未確認
		if (file->rtrack) {
			*(int *)addr = file->rtrack->usrbuf.count;
		} else {
			*(int *)addr = 0;
		}
		break;

	case FIOASYNC:
		if (*(int *)addr) {
			file->async_audio = curproc->p_pid;
			if (file->ptrack && file->ptrack->sih_wr == NULL) {
				file->ptrack->sih_wr = softint_establish(
				    SOFTINT_SERIAL | SOFTINT_MPSAFE,
				    audio_softintr_wr, file);
			}
			DPRINTF(2, "%s: FIOASYNC pid %d\n", __func__,
			    file->async_audio);
		} else {
			file->async_audio = 0;
			if (file->ptrack && file->ptrack->sih_wr) {
				softint_disestablish(file->ptrack->sih_wr);
				file->ptrack->sih_wr = NULL;
			}
			DPRINTF(2, "%s: FIOASYNC off\n", __func__);
		}
		break;

	case AUDIO_FLUSH:
		// このコマンドはすべての再生と録音を停止し、すべてのキューの
		// バッファをクリアし、エラーカウンタをリセットし、そして
		// 現在のサンプリングモードで再生と録音を再開します。
		audio_file_clear(sc, file);
		break;

	/*
	 * Number of read (write) samples dropped.  We don't know where or
	 * when they were dropped.
	 */
	// サンプル数と言ってるがバイト数のようだ
	case AUDIO_RERROR:
		// ここでカウントしてるのは録音ミキサからこのトラックに
		// 渡すことができなかったフレーム数なので、バイト数としては
		// 内部形式換算だが、そもそもこのエラーカウント自体がどこで
		// どのように落としたものかは問わないとあるのでこれでいい。
		if (file->rtrack) {
			*(int *)addr = frametobyte(&file->rtrack->inputfmt,
			    file->rtrack->dropframes);
		}
		break;

	case AUDIO_PERROR:
		// XXX 未実装
		*(int *)addr = 0;
		break;

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
	case AUDIO_WSEEK:
		if (file->ptrack)
			*(u_long *)addr = file->ptrack->usrbuf.count;
		break;

	case AUDIO_SETINFO:
		error = audio_file_setinfo(sc, file, (struct audio_info *)addr);
		if (error)
			break;
		/* update last_ai if /dev/sound */
		/* XXX これたぶん違うんじゃないかなあ */
		if (ISDEVSOUND(dev))
			error = audiogetinfo(sc, &sc->sc_ai, 0, file);
		break;

	case AUDIO_GETINFO:
		error = audiogetinfo(sc, (struct audio_info *)addr, 1, file);
		break;

	case AUDIO_GETBUFINFO:
		error = audiogetinfo(sc, (struct audio_info *)addr, 0, file);
		break;

	case AUDIO_DRAIN:
		// audio_drain 呼んで
		// 最後の再生トラックなら hw_if->drain をコールする
		error = audio_drain(sc, file->ptrack);
		break;

	case AUDIO_GETDEV:
		error = sc->hw_if->getdev(sc->hw_hdl, (audio_device_t *)addr);
		break;

	case AUDIO_GETENC:
		error = audio_getenc(sc, addr);
		break;

	case AUDIO_GETFD:
		// file が現在 Full Duplex かどうかを返す。
		if (file->ptrack && file->rtrack)
			*(int *)addr = 1;
		else
			*(int *)addr = 0;
		break;

	case AUDIO_SETFD:
		// 状態を変えないのなら黙って成功しておく。
		// Half -> Full は従来通り ENOTTY。
		// Full -> Half は禁止なので EINVAL。
		fd = *(int *)addr;
		if (file->ptrack && file->rtrack) {
			if (fd == 0)
				return EINVAL;
		} else {
			if (fd)
				return ENOTTY;
		}
		break;

	case AUDIO_GETPROPS:
		*(int *)addr = audio_get_props(sc);
		break;

	default:
		if (sc->hw_if->dev_ioctl) {
			error = sc->hw_if->dev_ioctl(sc->hw_hdl,
			    cmd, addr, flag, l);
		} else {
			DPRINTF(2, "audio_ioctl: unknown ioctl\n");
			error = EINVAL;
		}
		break;
	}
	DPRINTF(2, "audio_ioctl(%lu,'%c',%lu)%s result %d\n",
		 IOCPARM_LEN(cmd), (char)IOCGROUP(cmd), cmd&0xff, ioctlname,
		 error);
	return error;
}

int
audio_poll(struct audio_softc *sc, int events, struct lwp *l,
	audio_file_t *file)
{
	int revents;

	KASSERT(mutex_owned(sc->sc_lock));

	DPRINTF(2, "audio_poll: events=0x%x mode=%d\n", events, file->mode);

	// XXX 動作未確認

	// HW が Half Duplex でも、ソフトウェアレイヤは常に Full Duplex。
	// でいいはず。

	revents = 0;
	if (events & (POLLIN | POLLRDNORM)) {
		if (file->rtrack) {
			audio_ring_t *usrbuf = &file->rtrack->usrbuf;
			if (usrbuf->count > 0)
				revents |= events & (POLLIN | POLLRDNORM);
		}
	}
	if (events & (POLLOUT | POLLWRNORM)) {
		if (file->ptrack) {
			audio_ring_t *usrbuf = &file->ptrack->usrbuf;
			if (usrbuf->count < usrbuf->capacity)
				revents |= events & (POLLOUT | POLLWRNORM);
		}
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
	audio_ring_t *usrbuf;

	file = kn->kn_hook;
	sc = file->sc;

	// XXX READ 可能な時しかここ来ないのかな?

	// XXX 仕様がよくわからんけど、
	// 録音バッファのバイト数を調べるだけじゃいかんのか?

	mutex_enter(sc->sc_intr_lock);
	if (file->rtrack) {
		usrbuf = &file->rtrack->usrbuf;
		kn->kn_data = usrbuf->count;
	}
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
	audio_ring_t *usrbuf;

	file = kn->kn_hook;
	sc = file->sc;

	// XXX WRITE 可能な時しかここ来ないのかな?

	// XXX 仕様がよくわからんけど、
	// 再生バッファの空きバイト数を調べるだけじゃいかんのか?

	mutex_enter(sc->sc_intr_lock);
	if (file->ptrack) {
		usrbuf = &file->ptrack->usrbuf;
		kn->kn_data = usrbuf->capacity - usrbuf->count;
	}
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

	DPRINTF(2, "audio_mmap: off=%lld, prot=%d\n", (long long)(*offp), prot);
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

	DPRINTF(2, "audiostartr\n");

	if (!audio_can_capture(sc))
		return EINVAL;

	// ここで録音ループ起動
	sc->sc_rbusy = true;

	return error;
}

// 再生ループを開始する。
// 再生ループがとまっている時(!sc_pbusyの時)だけ呼び出せる。
// mixer->busy とは独立。
int
audiostartp(struct audio_softc *sc)
{
	printf("%s not used\n", __func__);
	return 0;
}

// audio_pint_silence いるかどうかは今後

/*
 * SIGIO derivery for record.
 *
 * o softintr cookie for record is one per device.  Initialize on attach()
 *   and release on detach().
// o audio_rmixer_process で全録音トラックへの分配が終ったところで
//   1回ソフトウエア割り込みを要求。
// o ソフトウェア割り込みで、すべての async 録音トラックに対して SIGIO を
//   投げる。
// o このため
//  - audio_rmixer_process -> open and set FAIOASYNC -> audoi_softintr_rd
//    の順でオープンされたトラックについては、録音データがまだ到着していない
//    にも関わらずシグナルが届くことになるがこれは許容する。
 */

// 録音時は rmixer_process から (リスナーが何人いても) 1回だけこれが呼ばれる
// ので、async 設定しているプロセス全員にここで SIGIO を配送すればよい。
static void
audio_softintr_rd(void *cookie)
{
	struct audio_softc *sc = cookie;
	audio_file_t *f;
	proc_t *p;
	pid_t pid;

	mutex_enter(sc->sc_lock);
	// XXX 元々ここで rchan を broadcast してた
	selnotify(&sc->sc_rsel, 0, NOTE_SUBMIT);
	SLIST_FOREACH(f, &sc->sc_files, entry) {
		pid = f->async_audio;
		if (pid != 0) {
			TRACEF(f, "sending SIGIO %d", pid);
			mutex_enter(proc_lock);
			if ((p = proc_find(pid)) != NULL)
				psignal(p, SIGIO);
			mutex_exit(proc_lock);
		}
	}
	mutex_exit(sc->sc_lock);
}

/*
 * SIGIO derivery for playback.
 *
 * o softintr cookie for playback is one per play track.  Initialize on
 *   FIOASYNC(on) and release FIOASYNC(off) or close.
//   オープンで初期化/クローズで解放にしてもいいけど、再生オープンする人
//   に比べて ASYNC 使う人は少ないと思うので。
// o 再生ミキサで ASYNC トラックのバッファを消費した際に lowat を下回ったら、
//   ソフトウェア割り込みを要求。
// o ソフトウェア割り込みで、このトラックに対して SIGIO を投げる。
 */

static void
audio_softintr_wr(void *cookie)
{
	struct audio_softc *sc;
	audio_file_t *file;
	audio_file_t *f;
	proc_t *p;
	pid_t pid;
	bool found;

	file = cookie;
	sc = file->sc;
	TRACEF(file, "start");

	// 自分自身がまだ有効かどうか調べる
	found = false;
	mutex_enter(sc->sc_lock);
	SLIST_FOREACH(f, &sc->sc_files, entry) {
		if (f == file) {
			found = true;
			break;
		}
	}
	if (found) {
		selnotify(&sc->sc_wsel, 0, NOTE_SUBMIT);
		pid = file->async_audio;
		TRACEF(file, "sending SIGIO %d", pid);
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
// ここに audio_pintr

/*
 * Called from HW driver module on completion of DMA input.
 * Mark it as input in the ring buffer (fiddle pointers).
 * Do a wakeup if necessary.
 */
// ここに audio_rintr

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

// XXX 実際にはチェックといいつつ、古いエンコーディングの翻訳も兼ねていて
// なんかもう何がなにやらである。
static int
audio_check_params2(audio_format2_t *f2)
{
	struct audio_params p;
	int error;

	p = format2_to_params(f2);
	error = audio_check_params(&p);
	*f2 = params_to_format2(&p);

	return error;
}

// 周波数を選択する。
// XXX アルゴリズムどうすっかね
// 48kHz、44.1kHz をサポートしてれば優先して使用。
// そうでなければとりあえず最高周波数にしとくか。
static int
audio_select_freq(const struct audio_format *fmt)
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
audio_hw_config_fmt(struct audio_softc *sc, audio_format2_t *cand, int mode)
{
	// 分かりやすさのため、しばらくどっち使ったか表示しとく
	if (sc->hw_if->query_format) {
		aprint_normal_dev(sc->dev, "use new query_format method\n");
		return audio_hw_config_by_format(sc, cand, mode);
	} else {
		aprint_normal_dev(sc->dev, "use old set_param method\n");
		return audio_hw_config_by_encoding(sc, cand, mode);
	}
}

// HW フォーマットを query_format を使って決定する
static int
audio_hw_config_by_format(struct audio_softc *sc, audio_format2_t *cand,
	int mode)
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
			DPRINTF(1, "fmt[%d] skip; INVALID\n", i);
			continue;
		}
		if ((fmt->mode & mode) == 0) {
			DPRINTF(1, "fmt[%d] skip; mode not match %d\n", i, mode);
			continue;
		}

		if (fmt->encoding != AUDIO_ENCODING_SLINEAR_NE) {
			DPRINTF(1, "fmt[%d] skip; enc=%d\n", i, fmt->encoding);
			continue;
		}
		if ((fmt->precision != AUDIO_INTERNAL_BITS ||
		     fmt->validbits != AUDIO_INTERNAL_BITS)) {
			DPRINTF(1, "fmt[%d] skip; precision %d/%d\n", i,
			    fmt->validbits, fmt->precision);
			continue;
		}
		if (fmt->channels < cand->channels) {
			DPRINTF(1, "fmt[%d] skip; channels %d < %d\n", i,
			    fmt->channels, cand->channels);
			continue;
		}
		int freq = audio_select_freq(fmt);
		// XXX うーん
		if (freq < cand->sample_rate) {
			DPRINTF(1, "fmt[%d] skip; frequency %d < %d\n", i,
			    freq, cand->sample_rate);
			continue;
		}

		// cand 更新
		cand->channels = fmt->channels;
		cand->sample_rate = freq;
		DPRINTF(1, "fmt[%d] cand ch=%d freq=%d\n", i,
		    cand->channels, cand->sample_rate);
	}

	if (cand->sample_rate == 0) {
		DPRINTF(1, "%s no fmt\n", __func__);
		return ENXIO;
	}
	DPRINTF(1, "%s selected: ch=%d freq=%d\n", __func__,
	    cand->channels, cand->sample_rate);
	return 0;
}

// HW フォーマットを query_format を使わずに決定する
// とても query_format を書き起こせないような古いドライバ向けなので、
// 探索パラメータはこれでよい。
static int
audio_hw_config_by_encoding(struct audio_softc *sc, audio_format2_t *cand,
	int mode)
{
	static int freqlist[] = { 48000, 44100, 22050, 11025, 8000, 4000 };
	audio_format2_t fmt;
	int ch, i, freq;
	int error;

	fmt.encoding  = AUDIO_ENCODING_SLINEAR_LE;
	fmt.precision = AUDIO_INTERNAL_BITS;
	fmt.stride    = AUDIO_INTERNAL_BITS;

	for (ch = 2; ch > 0; ch--) {
		for (i = 0; i < __arraycount(freqlist); i++) {
			freq = freqlist[i];

			fmt.channels = ch;
			fmt.sample_rate = freq;
			if ((mode & AUMODE_PLAY) != 0)
				sc->sc_phwfmt = fmt;
			if ((mode & AUMODE_RECORD) != 0)
				sc->sc_rhwfmt = fmt;
			error = audio_set_params(sc, mode);
			if (error == 0) {
				// 設定できたのでこれを採用
				*cand = fmt;
				DPRINTF(1, "%s selected: ch=%d freq=%d\n",
				    __func__,
				    fmt.channels,
				    fmt.sample_rate);
				return 0;
			}
			DPRINTF(1, "%s trying ch=%d freq=%d failed\n",
			    __func__,
			    fmt.channels,
			    fmt.sample_rate);
		}
	}
	return ENXIO;
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
audio_hw_config(struct audio_softc *sc, int is_indep)
{
	audio_format2_t fmt;
	int mode;
	int error;

	if (is_indep) {
		/* independent devices */
		if (sc->sc_can_playback) {
			error = audio_hw_config_fmt(sc, &sc->sc_phwfmt,
			    AUMODE_PLAY);
			if (error)
				sc->sc_can_playback = false;
		}
		if (sc->sc_can_capture) {
			error = audio_hw_config_fmt(sc, &sc->sc_rhwfmt,
			    AUMODE_RECORD);
			if (error)
				sc->sc_can_capture = false;
		}
	} else {
		/* not independent devices */
		mode = 0;
		if (sc->sc_can_playback)
			mode |= AUMODE_PLAY;
		if (sc->sc_can_capture)
			mode |= AUMODE_RECORD;
		error = audio_hw_config_fmt(sc, &fmt, mode);
		if (error) {
			sc->sc_can_playback = false;
			sc->sc_can_capture = false;
			return error;
		}
		sc->sc_phwfmt = fmt;
		sc->sc_rhwfmt = fmt;
	}

	mode = 0;
	if (sc->sc_can_playback)
		mode |= AUMODE_PLAY;
	if (sc->sc_can_capture)
		mode |= AUMODE_RECORD;

	error = audio_set_params(sc, mode);
	if (error)
		return error;

	return error;
}

// audioinfo の各パラメータについて。
//
// ai.{play,record}.sample_rate		(R/W)
// ai.{play,record}.encoding		(R/W)
// ai.{play,record}.precision		(R/W)
// ai.{play,record}.channels		(R/W)
//	再生/録音フォーマット。
//	現在有効でないトラック側は無視する。
//
// ai.mode				(R/W)
//	再生/録音モード。AUMODE_*
//	XXX ai.mode = PLAY だったところに ai.mode = RECORD とか指定すると
//	    何が起きるか。何が起きるべきか。
//
// ai.{hiwat,lowat}			(R/W)
//	再生トラックの hiwat/lowat。単位はブロック。
//
// ai.{play,record}.gain		(R/W)
//	ボリューム。0-255。
//	N7 以前は HW ミキサのボリュームと連動していた。
//	N8 はこれをソフトウェアボリュームにしようとしてバグっている。
//	理想的にはソフトウェアボリュームでもいいような気がするが。
//
// ai.{play,record}.balance		(R/W)
//	左右バランス。0-64。32 が中心。
//	N7 以前は gain と balance をセットで扱うようなインタフェースだったが
//	N8 ではバランスは HW ミキサーに任せたまま。
//	理想的にはソフトウェアバランスでもいいような気がするが。
//
// ai.{play,record}.port		(R/W)
//	入出力ポート。これは HW ミキサー情報。
//
// ai.monitor_gain			(R/W)
//	録音モニターゲイン(?)。これは HW ミキサー情報。
//
// ai.{play,record}.pause		(R/W)
//	トラックの一時停止なら non-zero。
//
// ai.{play,record}.seek		(R/-)
//	バッファ上のポインタ位置。usrbuf にすべか。
//
// ai.{play,record}.avail_ports		(R/-)
//	これは HW ミキサー情報。
//
// ai.{play,record}.buffer_size		(R/-)
//	バッファサイズ[byte]。usrbuf をさすのでいいか。
//
// ai.{play,record}.samples		(R/-)
//	usrbuf に受け取った / usrbuf から引き渡したサンプル数
//	or バイト数。このへん単位の定義が曖昧。
//
// ai.{play,record}.eof			(R/-)
//	EOF に到達した回数?
//
// ai.{play,record}.error		(R/-)
//	アンダーフロー/オーバーフローの起きたら non-zero。
//
// ai.{play,record}.waiting		(R/-)
//	他のプロセスが open 待ちしていれば non-zero。
//	今は起きないので常にゼロ。
//
// ai.{play,record}.open		(R/-)
//	オープンされていれば non-zero。
//
// ai.{play,record}.active		(R/-)
//	IO がアクティブなら non-zero。
//
// ai.blocksize				(R/-)
//	audioio.h には HW ブロックサイズとコメントがあるが、
//	今は usrbuf の1ブロックサイズ[byte]にしたほうがよさげ。
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

// ai に基づいて file の両トラックを諸々セットする。
// ai のうち初期値のままのところは sc_[pr]params, sc_[pr]pause が使われる。
// セットできれば sc_[pr]params, sc_[pr]pause も更新する。
// オープン時に呼ばれる時は file はまだ sc_files には繋がっていない。
static int
audio_file_setinfo(struct audio_softc *sc, audio_file_t *file,
	const struct audio_info *ai)
{
	const struct audio_prinfo *pi;
	const struct audio_prinfo *ri;
	audio_track_t *play;
	audio_track_t *rec;
	audio_format2_t pfmt;
	audio_format2_t rfmt;
	bool ppause;
	bool rpause;
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
	int saved_mode;
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	pi = &ai->play;
	ri = &ai->record;
	pchanges = 0;
	rchanges = 0;

	play = file->ptrack;
	rec = file->rtrack;

#if AUDIO_DEBUG > 1
	char buf[80];
	int n = 0;
#define SNPRINTF(fmt...) do {	\
	n += snprintf(buf + n, sizeof(buf) - n, fmt);	\
} while (0)

	if (SPECIFIED(pi->encoding)) {
		SNPRINTF(" enc=%d", pi->encoding);
		pchanges++;
	}
	if (SPECIFIED(pi->precision)) {
		SNPRINTF(" prec=%d", pi->precision);
		pchanges++;
	}
	if (SPECIFIED(pi->channels)) {
		SNPRINTF(" ch=%d", pi->channels);
		pchanges++;
	}
	if (SPECIFIED(pi->sample_rate)) {
		SNPRINTF(" freq=%d", pi->sample_rate);
		pchanges++;
	}
	if (pchanges) {
		printf("%s play:%s\n", __func__, buf);
		n = 0;
		pchanges = 0;
	}

	if (SPECIFIED(ri->encoding)) {
		SNPRINTF(" enc=%d", ri->encoding);
		rchanges++;
	}
	if (SPECIFIED(ri->precision)) {
		SNPRINTF(" prec=%d", ri->precision);
		rchanges++;
	}
	if (SPECIFIED(ri->channels)) {
		SNPRINTF(" ch=%d", ri->channels);
		rchanges++;
	}
	if (SPECIFIED(ri->sample_rate)) {
		SNPRINTF(" freq=%d", ri->sample_rate);
		rchanges++;
	}
	if (rchanges) {
		printf("%s rec:%s\n", __func__, buf);
		rchanges = 0;
	}

	if (SPECIFIED(ai->mode))
		printf("%s mode=%d\n", __func__, ai->mode);
	if (SPECIFIED(ai->hiwat))
		printf("%s hiwat=%d\n", __func__, ai->hiwat);
	if (SPECIFIED(ai->lowat))
		printf("%s lowat=%d\n", __func__, ai->lowat);
	if (SPECIFIED(ai->play.gain))
		printf("%s play.gain=%d\n", __func__, ai->play.gain);
	if (SPECIFIED(ai->record.gain))
		printf("%s record.gain=%d\n", __func__, ai->record.gain);
	if (SPECIFIED_CH(ai->play.balance))
		printf("%s play.balance=%d\n", __func__, ai->play.balance);
	if (SPECIFIED_CH(ai->record.balance))
		printf("%s record.balance=%d\n", __func__, ai->record.balance);
	if (SPECIFIED(ai->play.port))
		printf("%s play.port=%d\n", __func__, ai->play.port);
	if (SPECIFIED(ai->record.port))
		printf("%s record.port=%d\n", __func__, ai->record.port);
	if (SPECIFIED(ai->monitor_gain))
		printf("%s monitor_gain=%d\n", __func__, ai->monitor_gain);
	if (SPECIFIED_CH(ai->play.pause))
		printf("%s play.pause=%d\n", __func__, ai->play.pause);
	if (SPECIFIED_CH(ai->record.pause))
		printf("%s record.pause=%d\n", __func__, ai->record.pause);
#endif

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
	saved_mode = file->mode;

	/* Set default value */
	// sc_[pr]params, sc_[pr]pause が現在の /dev/sound の設定値
	pfmt = sc->sc_pparams;
	rfmt = sc->sc_rparams;
	ppause = sc->sc_ppause;
	rpause = sc->sc_rpause;

	/* Overwrite if specified */
	mode = file->mode;
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

	// ここから mode は変更後の希望するモード。
	if (play && (mode & AUMODE_PLAY) != 0) {
		pchanges = audio_file_setinfo_check(&pfmt, pi);
		if (pchanges == -1)
			return EINVAL;
		if (SPECIFIED_CH(pi->pause))
			ppause = pi->pause;
	}
	if (rec && (mode & AUMODE_RECORD) != 0) {
		rchanges = audio_file_setinfo_check(&rfmt, ri);
		if (rchanges == -1)
			return EINVAL;
		if (SPECIFIED_CH(ri->pause))
			ppause = ri->pause;
	}

	if (SPECIFIED(ai->mode) || pchanges || rchanges) {
		audio_file_clear(sc, file);
#ifdef AUDIO_DEBUG
		char modebuf[64];
		snprintb(modebuf, sizeof(modebuf),
		    "\177\020" "b\0PLAY\0" "b\2PLAY_ALL\0" "b\1RECORD\0",
		    mode);
		printf("setting mode to %s (pchanges=%d rchanges=%d)\n",
		    modebuf, pchanges, rchanges);
		if (pchanges)
			audio_print_format2("setting play mode:", &pfmt);
		if (rchanges)
			audio_print_format2("setting rec  mode:", &rfmt);
#endif
	}

	/* Set */
	error = 0;
	file->mode = mode;
	if (play) {
		play->is_pause = ppause;
		error = audio_file_setinfo_set(play, pi, pchanges, &pfmt,
		    (mode & (AUMODE_PLAY | AUMODE_PLAY_ALL)));
		if (error)
			goto abort1;

		/* update sticky parameters */
		sc->sc_pparams = pfmt;
		sc->sc_ppause = ppause;
	}
	if (rec) {
		rec->is_pause = rpause;
		error = audio_file_setinfo_set(rec, ri, rchanges, &rfmt,
		    (mode & AUMODE_RECORD));
		if (error)
			goto abort2;

		/* update sticky parameters */
		sc->sc_rparams = rfmt;
		sc->sc_rpause = rpause;
	}

	return 0;

	/* Rollback */
abort2:
	if (error != ENOMEM) {
		rec->is_pause = saved_rpause;
		AUDIO_INITINFO(&bk);
		bk.record.gain = saved_rvolume;
		audio_file_setinfo_set(rec, &bk.record, true, &saved_rfmt,
		    (saved_mode & AUMODE_RECORD));
	}
abort1:
	if (play && error != ENOMEM) {
		play->is_pause = saved_ppause;
		AUDIO_INITINFO(&bk);
		bk.play.gain = saved_pvolume;
		audio_file_setinfo_set(play, &bk.play, true, &saved_pfmt,
		    (saved_mode & (AUMODE_PLAY | AUMODE_PLAY_ALL)));
		sc->sc_pparams = saved_pfmt;
		sc->sc_ppause = saved_ppause;
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
		if (info->sample_rate < AUDIO_MIN_FREQUENCY)
			return -1;
		if (info->sample_rate > AUDIO_MAX_FREQUENCY)
			return -1;
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
		if (audio_check_params2(fmt) != 0) {
#if AUDIO_DEBUG > 1
			char fmtbuf[64];
			audio_format2_tostr(fmtbuf, sizeof(fmtbuf), fmt);
			DPRINTF(0, "%s failed: %s\n", __func__, fmtbuf);
#endif
			return -1;
		}
	}

	return changes;
}

// modechange なら track に mode, fmt を設定する。
// あと(modechangeに関わらず) info のソフトウェアパラメータも設定する。
// mode は再生トラックなら AUMODE_PLAY{,_ALL}、録音トラックなら AUMODE_RECORD
// のように該当するビットだけを渡すこと。
// 成功すれば0、失敗なら errno
// ここではロールバックしない。
static int
audio_file_setinfo_set(audio_track_t *track, const struct audio_prinfo *info,
	bool modechange, audio_format2_t *fmt, int mode)
{
	int error;

	if (modechange) {
		KASSERT(track);

		mutex_enter(track->mixer->sc->sc_intr_lock);
		track->mode = mode;
		error = audio_track_set_format(track, fmt);
		mutex_exit(track->mixer->sc->sc_intr_lock);
		if (error)
			return error;
	}

	if (SPECIFIED(info->gain)) {
		if (info->gain > AUDIO_MAX_GAIN) {
			DPRINTF(1, "%s: info.gain(%d) out of range\n",
			    __func__, info->gain);
			return EINVAL;
		}
		track->volume = audio_volume_to_inner(info->gain);
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
	struct audio_prinfo *pi;
	struct audio_prinfo *ri;
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

	pi = &ai->play;
	ri = &ai->record;
	pbusy = sc->sc_pbusy;
	rbusy = sc->sc_rbusy;
	cleared = false;
	error = 0;

	/* XXX shut up gcc */
	pport = 0;
	rport = 0;

	if (SPECIFIED(pi->port) || SPECIFIED(ri->port)) {
		audio_hw_clear(sc);
		cleared = true;
	}
	if (SPECIFIED(pi->port)) {
		pport = au_get_port(sc, &sc->sc_outports);
		error = au_set_port(sc, &sc->sc_outports, pi->port);
		if (error)
			goto abort1;
	}
	if (SPECIFIED(ri->port)) {
		rport = au_get_port(sc, &sc->sc_inports);
		error = au_set_port(sc, &sc->sc_inports, ri->port);
		if (error)
			goto abort2;
	}

	if (SPECIFIED_CH(pi->balance)) {
		au_get_gain(sc, &sc->sc_outports, &pgain, &pbalance);
		error = au_set_gain(sc, &sc->sc_outports, pgain, pi->balance);
		if (error)
			goto abort3;
	}
	if (SPECIFIED_CH(ri->balance)) {
		au_get_gain(sc, &sc->sc_inports, &rgain, &rbalance);
		error = au_set_gain(sc, &sc->sc_inports, rgain, ri->balance);
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
	if (SPECIFIED_CH(ri->balance))
		au_set_gain(sc, &sc->sc_inports, rgain, rbalance);
abort3:
	if (SPECIFIED_CH(pi->balance))
		au_set_gain(sc, &sc->sc_outports, pgain, pbalance);
abort2:
	if (SPECIFIED(ri->port))
		au_set_port(sc, &sc->sc_inports, rport);
abort1:
	if (SPECIFIED(pi->port))
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
audio_set_params(struct audio_softc *sc, int setmode)
{
	audio_params_t pp, rp;
	stream_filter_list_t pfilters, rfilters;
	audio_filter_reg_t pfilters2, rfilters2;
	int error;
	int usemode;
	bool use_set_params2;

	// set_params2 が定義されてればそっちを使う
	use_set_params2 = (sc->hw_if->set_params2 != NULL);
	if (use_set_params2)
		DPRINTF(2, "%s use_set_params2\n", __func__);

	usemode = setmode;
	pp = format2_to_params(&sc->sc_phwfmt);
	rp = format2_to_params(&sc->sc_rhwfmt);

	if (use_set_params2) {
		memset(&pfilters2, 0, sizeof(pfilters2));
		memset(&rfilters2, 0, sizeof(rfilters2));
	} else {
		memset(&pfilters, 0, sizeof(pfilters));
		memset(&rfilters, 0, sizeof(rfilters));
		pfilters.append = stream_filter_list_append;
		pfilters.prepend = stream_filter_list_prepend;
		pfilters.set = stream_filter_list_set;
		rfilters.append = stream_filter_list_append;
		rfilters.prepend = stream_filter_list_prepend;
		rfilters.set = stream_filter_list_set;
	}

	mutex_enter(sc->sc_lock);
	if (use_set_params2) {
		error = sc->hw_if->set_params2(sc->hw_hdl, setmode, usemode,
		    &pp, &rp, &pfilters2, &rfilters2);
		if (error) {
			mutex_exit(sc->sc_lock);
			DPRINTF(1, "%s: set_params2 failed with %d\n",
			    __func__, error);
			return error;
		}
	} else {
		error = sc->hw_if->set_params(sc->hw_hdl, setmode, usemode,
		    &pp, &rp, &pfilters, &rfilters);
		if (error) {
			mutex_exit(sc->sc_lock);
			DPRINTF(1, "%s: set_params failed with %d\n",
			    __func__, error);
			return error;
		}
	}

	if (sc->hw_if->commit_settings) {
		error = sc->hw_if->commit_settings(sc->hw_hdl);
		if (error) {
			mutex_exit(sc->sc_lock);
			DPRINTF(1, "%s: commit_settings failed with %d\n",
			    __func__, error);
			return error;
		}
	}
	mutex_exit(sc->sc_lock);

	sc->sc_phwfmt = params_to_format2(&pp);
	sc->sc_rhwfmt = params_to_format2(&rp);
	if (use_set_params2) {
		sc->sc_xxx_pfilreg = pfilters2;
		sc->sc_xxx_rfilreg = rfilters2;
	}

	return 0;
}

static int
audiogetinfo(struct audio_softc *sc, struct audio_info *ai, int need_mixerinfo,
	audio_file_t *file)
{
	const struct audio_hw_if *hw;
	struct audio_prinfo *ri, *pi;
	audio_track_t *ptrack;
	audio_track_t *rtrack;
	int gain;

	KASSERT(mutex_owned(sc->sc_lock));

	ri = &ai->record;
	pi = &ai->play;
	hw = sc->hw_if;
	if (hw == NULL)		/* HW has not attached */
		return ENXIO;

	ptrack = file->ptrack;
	rtrack = file->rtrack;

	memset(ai, 0, sizeof(*ai));

	if (ptrack) {
		pi->sample_rate = ptrack->inputfmt.sample_rate;
		pi->channels    = ptrack->inputfmt.channels;
		pi->precision   = ptrack->inputfmt.precision;
		pi->encoding    = ptrack->inputfmt.encoding;
	} else {
		pi->sample_rate = sc->sc_pparams.sample_rate;
		pi->channels    = sc->sc_pparams.channels;
		pi->precision   = sc->sc_pparams.precision;
		pi->encoding    = sc->sc_pparams.encoding;
	}
	if (rtrack) {
		ri->sample_rate = rtrack->outputbuf.fmt.sample_rate;
		ri->channels    = rtrack->outputbuf.fmt.channels;
		ri->precision   = rtrack->outputbuf.fmt.precision;
		ri->encoding    = rtrack->outputbuf.fmt.encoding;
	} else {
		ri->sample_rate = sc->sc_rparams.sample_rate;
		ri->channels    = sc->sc_rparams.channels;
		ri->precision   = sc->sc_rparams.precision;
		ri->encoding    = sc->sc_rparams.encoding;
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
		pi->seek = ptrack->usrbuf.top;
		pi->samples = ptrack->inputcounter;
		pi->eof = ptrack->eofcounter;
		pi->pause = ptrack->is_pause;
		pi->error = 0;			// XXX
		pi->waiting = 0;		/* open never hangs */
		pi->open = 1;
		pi->active = sc->sc_pbusy;
		pi->buffer_size = ptrack->usrbuf.capacity;
	}
	if (rtrack) {
		ri->seek = rtrack->usrbuf.top;
		ri->samples = rtrack->inputcounter;
		ri->eof = 0;
		ri->pause = rtrack->is_pause;
		ri->error = 0;			// XXX
		ri->waiting = 0;		/* open never hangs */
		ri->open = 1;
		ri->active = sc->sc_rbusy;
		ri->buffer_size = rtrack->usrbuf.capacity;
	}

	// XXX 再生と録音でチャンネル数が違う場合があるので
	// ブロックサイズは同じにはならないのだが、
	// もう仕方ないので異なる場合は再生側で代表するか?
	audio_track_t *track;
	track = ptrack ?: rtrack;
	ai->blocksize = track->usrbuf_blksize;
	ai->mode = file->mode;
	/* hiwat, lowat are meaningless in current implementation */
	// XXX inp_thres あたりと一緒になんとかしたほうがいいかも
	ai->hiwat = NBLKOUT;
	ai->lowat = 1;

	if (need_mixerinfo) {
		pi->port = au_get_port(sc, &sc->sc_outports);
		ri->port = au_get_port(sc, &sc->sc_inports);

		pi->avail_ports = sc->sc_outports.allports;
		ri->avail_ports = sc->sc_inports.allports;

		if (ptrack) {
			au_get_gain(sc, &sc->sc_outports, &gain, &pi->balance);
			pi->gain = audio_volume_to_outer(ptrack->volume);
		}
		if (rtrack) {
			au_get_gain(sc, &sc->sc_inports, &gain, &ri->balance);
			ri->gain = audio_volume_to_outer(rtrack->volume);
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
	int error;

	DPRINTF(2, "query_encoding[%d] ", ae->index);
	if (ae->index >= 0) {
		error = sc->hw_if->query_encoding(sc->hw_hdl, ae);
	} else {
		error = EINVAL;
	}

#if AUDIO_DEBUG > 1
	if (error) {
		DPRINTF(2, "error=%d\n", error);
	} else {
		DPRINTF(2, "name=\"%s\" enc=", ae->name);
		if (ae->encoding < __arraycount(encoding_names)) {
			DPRINTF(2, "%s", encoding_names[ae->encoding]);
		} else {
			DPRINTF(2, "?(%d)", ae->encoding);
		}
		DPRINTF(2, " prec=%d flags=%d(%s)\n",
		    ae->precision,
		    ae->flags,
		    ae->flags == 0 ? "native" : "EMULATED");
	}
#endif
	return error;
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

// デバッグ目的なのでボリュームは内部表現(0..256)
static int
audio_sysctl_volume(SYSCTLFN_ARGS)
{
	struct sysctlnode node;
	struct audio_softc *sc;
	int t, error;

	node = *rnode;
	sc = node.sysctl_data;

	if (sc->sc_pmixer)
		t = sc->sc_pmixer->volume;
	else
		t = -1;
	node.sysctl_data = &t;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;

	if (sc->sc_pmixer == NULL)
		return EINVAL;
	if (t < 0)
		return EINVAL;

	sc->sc_pmixer->volume = t;
	return 0;
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
	if (sc->sc_pbusy)
		audio2_halt_output(sc);
	if (sc->sc_rbusy)
		audio2_halt_input(sc);
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

static void
audio_format2_tostr(char *buf, size_t bufsize, const audio_format2_t *fmt)
{
	int n;

	n = 0;
	if (fmt->encoding < __arraycount(encoding_names)) {
		n += snprintf(buf + n, bufsize - n, "%s",
		    encoding_names[fmt->encoding]);
	} else {
		n += snprintf(buf + n, bufsize - n, "unknown_encoding=%d",
		    fmt->encoding);
	}
	if (fmt->precision == fmt->stride) {
		n += snprintf(buf + n, bufsize - n, " %dbit", fmt->precision);
	} else {
		n += snprintf(buf + n, bufsize - n, " %d/%dbit",
			fmt->precision, fmt->stride);
	}

	snprintf(buf + n, bufsize - n, " %uch %uHz",
	    fmt->channels, fmt->sample_rate);
}

#ifdef AUDIO_DEBUG
static void
audio_print_format2(const char *s, const audio_format2_t *fmt)
{
	char fmtstr[64];

	audio_format2_tostr(fmtstr, sizeof(fmtstr), fmt);
	printf("%s %s\n", s, fmtstr);
}
#endif

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

	DPRINTF(1, "mixer_open: flags=0x%x sc=%p\n", flags, sc);

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

	DPRINTF(1, "mixer_close: sc %p\n", sc);
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

	DPRINTF(2, "mixer_ioctl(%lu,'%c',%lu)\n",
		 IOCPARM_LEN(cmd), (char)IOCGROUP(cmd), cmd&0xff);
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
		DPRINTF(2, "AUDIO_GETDEV\n");
		error = hw->getdev(sc->hw_hdl, (audio_device_t *)addr);
		break;

	case AUDIO_MIXER_DEVINFO:
		DPRINTF(2, "AUDIO_MIXER_DEVINFO\n");
		((mixer_devinfo_t *)addr)->un.v.delta = 0; /* default */
		error = audio_query_devinfo(sc, (mixer_devinfo_t *)addr);
		break;

	case AUDIO_MIXER_READ:
		DPRINTF(2, "AUDIO_MIXER_READ\n");
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
		DPRINTF(2, "AUDIO_MIXER_WRITE\n");
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
	DPRINTF(2, "mixer_ioctl(%lu,'%c',%lu) result %d\n",
		 IOCPARM_LEN(cmd), (char)IOCGROUP(cmd), cmd&0xff, error);
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
	DPRINTF(2, "au_set_gain: gain=%d balance=%d, l=%d r=%d\n",
		 gain, balance, l, r);

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

static void
unitscopy(mixer_devinfo_t *di, const char *name)
{
	strlcpy(di->un.v.units.name, name, sizeof(di->un.v.units.name));
}

static int
audio_query_devinfo(struct audio_softc *sc, mixer_devinfo_t *di)
{
	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->sc_static_nmixer_states == 0 || sc->sc_nmixer_states == 0)
		goto hardware;

	if (di->index >= sc->sc_static_nmixer_states - 1 &&
	    di->index < sc->sc_nmixer_states) {
		if (di->index == sc->sc_static_nmixer_states - 1) {
			di->mixer_class = sc->sc_static_nmixer_states -1;
			di->next = di->prev = AUDIO_MIXER_LAST;
			strlcpy(di->label.name, AudioCvirtchan,
			    sizeof(di->label.name));
			di->type = AUDIO_MIXER_CLASS;
		} else if ((di->index - sc->sc_static_nmixer_states) % 2 == 0) {
			di->mixer_class = sc->sc_static_nmixer_states -1;
			snprintf(di->label.name, sizeof(di->label.name),
			    AudioNdac"%d",
			    (di->index - sc->sc_static_nmixer_states) / 2);
			di->type = AUDIO_MIXER_VALUE;
			di->next = di->prev = AUDIO_MIXER_LAST;
			di->un.v.num_channels = 1;
			unitscopy(di, AudioNvolume);
		} else {
			di->mixer_class = sc->sc_static_nmixer_states -1;
			snprintf(di->label.name, sizeof(di->label.name),
			    AudioNmicrophone "%d",
			    (di->index - sc->sc_static_nmixer_states) / 2);
			di->type = AUDIO_MIXER_VALUE;
			di->next = di->prev = AUDIO_MIXER_LAST;
			di->un.v.num_channels = 1;
			unitscopy(di, AudioNvolume);
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
#include <dev/audio/aucodec_linear.c>
#include <dev/audio/aucodec_mulaw.c>


// x audioctl play.gain と mixerctl outputs.master は N7 では連動していたし
//   ドキュメントにもそう書いてある
// x 全般的に AUDIO_SETINFO どうすんだというのはある
//  - pause と他の設定変更を同時にするのはやめたほうがいい
//  - 全体的にロールバックできてなさそう
// x audio(4) に AUDIO_PERROR がない (RERROR はある)
// x audio(9) の allocm のプロトタイプがおかしい?
// x AUDIO_RERROR はサンプル数と読めるが、実際に返すのはバイト数。
// x lastinfo を全トラックが持っているが、ハードウェア部分は1つの lastinfo
//   に分離しないと、suspend/resume の時におかしくなっている。
// x SETINFO の gain はソフトゲインだが GETINFO はハードウェアのほうを
//   取得しているっぽい
// x gain の範囲チェックは誰がしてるんだ
// x suspend 中に uaudio が消えるケースはまた後で
// x audio_clear が最後の録音停止時には halt_input を呼んでいるが
//   再生停止については halt_output を呼んでいないけど、なんで?
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
// x PLAY モードで少なくともフィルタが設定されてない時はフレーム境界を
//   意識していないので、途切れる直前のフレームが不完全なものになりそう。
//   ただし認識できるような問題にはならなさそう。次フレームの開始が
//   ずれることはなさげ。
