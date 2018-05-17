// vi:set ts=8:
// C++ style comment is unformal comment (that is, comment for comment).

// TODO:
// x Tune up PLAY(_SYNC) mode?
// x Restore mmap.
// x Restore NetBSD8-like track volume control on mixerctl?
// x Restore NetBSD8-like multiuser mode.
// x Restore spkr* at audio?
// x Restore pad(4).
// x Restore alow.
// x Restore(?) suspend/resume.
// x Support software master volume? (mixerctl?)
// x Support all MD drivers.
//   Only auich/hdafg/eap/uaudio/vs are supported for now.
// x Support linear_LE/BE conversion for MD codec.
// x Support tc/bba.c (mulaw with 32bit stride)
// x Catch up userland programs such as audioctl(1).
// x No idea about channel mapping.
// x Documents (even in Japanese).
// x Documents (in English).
// x Purge all C++ style comments.
// x Reconsideration about sc_lock?
//   Many syscalls may no longer require a device lock?
// x Reconsideration about rsel/wel.
//   rsel/wsel はトラックごとに分けなくてよいか
//   今のままでも動作はしているが
// x Reconsideration about sih_rd.
//   sih_rd はトラックごとに分けなくてよいか
//   今のままでも動作はしているが
// x Reconsideration about overflow handling on track mixer.
//   トラックミキサでオーバーフローする場合の処理はあれでいいか
//   今は自動的にマスターボリュームを下げている。そして下げっぱなし。
// x better name for audio_file_t ?
// x better name for audio_filter_* ?
// x Improve sizeof(audio_track_t).

/*-
 * Copyright (c) 2008 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Andrew Doran.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 1991-1993 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the Computer Systems
 *	Engineering Group at Lawrence Berkeley Laboratory.
 * 4. Neither the name of the University nor of the Laboratory may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

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
 *	drain 			x	x	(Not used in AUDIO2)
 *	query_encoding		-	x	(Not used in AUDIO2)
 *	set_params 		-	x	(Obsoleted in AUDIO2)
 *	round_blocksize		-	x	(*1)
 *	commit_settings		-	x
 *	init_output 		x	x
 *	init_input 		x	x
 *	start_output 		x	x
 *	start_input 		x	x
 *	halt_output 		x	x
 *	halt_input 		x	x
 *	speaker_ctl 		x	x
 *	getdev 			-	x
 *	setfd 			-	x	(Not used in AUDIO2)
 *	set_port 		-	x
 *	get_port 		-	x
 *	query_devinfo 		-	x
 *	allocm 			-	-	Called at attach time
 *	freem 			-	-	Called at attach time
 *	round_buffersize 	-	x	(*1)
 *	mappage 		-	-	(Not used in AUDIO2)
 *	get_props 		-	x
 *	trigger_output 		x	x
 *	trigger_input 		x	x
 *	dev_ioctl 		-	x
 *	get_locks 		-	-	Called at attach time
 *	query_format		-	x	(Added in AUDIO2)
 *	set_format		-	x	(Added in AUDIO2)
 *
 * *1: round_blocksize and round_buffersize become to be called only at
 *  attach time.  That means it's unnecessary to hold thread lock.  However,
 *  some hardware drivers already have verified that it is held on these
 *  methods.  So I define that thread lock should be held as they did before.
 *  This rule should be reviewed later.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#ifdef _KERNEL_OPT
#include "audio.h"
#include "midi.h"
#endif

#if NAUDIO > 0

#ifdef _KERNEL
#define OLD_FILTER

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
#include <dev/audio/audiodef.h>
#include <dev/audio/aucodec.h>
#include <dev/audio/mulaw.h>

#include <machine/endian.h>

#include <uvm/uvm.h>

#include "ioconf.h"
#endif /* _KERNEL */

// デバッグレベルは
// 1: open/close/set_param等
// 2: read/write/ioctlシステムコールくらいまでは含む
// 3: 割り込み以外のTRACEも含む
// 4: 割り込み内のTRACEも含む (要 AUDIO_DEBUG_MLOG)
#define AUDIO_DEBUG	4

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
		audio_mlog_flush();	\
		printf(fmt);		\
	}				\
} while (0)
int	audiodebug = AUDIO_DEBUG;
#else
#define DPRINTF(n, fmt...)	do { } while (0)
#endif

#if AUDIO_DEBUG > 2
static void audio_vtrace(const char *, const char *, const char *, va_list);
static void audio_trace(const char *, const char *, ...)
	__printflike(2, 3);
static void audio_tracet(const char *, audio_track_t *, const char *, ...)
	__printflike(3, 4);
static void audio_tracef(const char *, audio_file_t *, const char *, ...)
	__printflike(3, 4);

static void
audio_vtrace(const char *funcname, const char *header, const char *fmt,
	va_list ap)
{
	char buf[256];
	int n;

	n = 0;
	buf[0] = '\0';
	n += snprintf(buf + n, sizeof(buf) - n, "%s %s", funcname, header);
	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);

	if (cpu_intr_p()) {
#if AUDIO_DEBUG >= 4 && defined(AUDIO_DEBUG_MLOG)
		// 割り込み中なら覚えておくだけ
		audio_mlog_printf("%s\n", buf);
#endif
	} else {
		audio_mlog_flush();
		printf("%s\n", buf);
	}
}

static void
audio_trace(const char *funcname, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	audio_vtrace(funcname, "", fmt, ap);
	va_end(ap);
}

static void
audio_tracet(const char *funcname, audio_track_t *track, const char *fmt, ...)
{
	char hdr[16];
	va_list ap;

	snprintf(hdr, sizeof(hdr), "#%d ", track->id);
	va_start(ap, fmt);
	audio_vtrace(funcname, hdr, fmt, ap);
	va_end(ap);
}

static void
audio_tracef(const char *funcname, audio_file_t *file, const char *fmt, ...)
{
	char hdr[32];
	char phdr[16], rhdr[16];
	va_list ap;

	phdr[0] = '\0';
	rhdr[0] = '\0';
	if (file->ptrack)
		snprintf(phdr, sizeof(phdr), "#%d", file->ptrack->id);
	if (file->rtrack)
		snprintf(rhdr, sizeof(rhdr), "#%d", file->rtrack->id);
	snprintf(hdr, sizeof(hdr), "{%s,%s} ", phdr, rhdr);

	va_start(ap, fmt);
	audio_vtrace(funcname, hdr, fmt, ap);
	va_end(ap);
}

struct audio_track_debugbuf {
	char outbuf[32];
	char freq[32];
	char chvol[32];
	char chmix[32];
	char codec[32];
	char usrbuf[32];
};

// track の各バッファの状態を buf に出力します。
static void
audio_track_bufstat(audio_track_t *track, struct audio_track_debugbuf *buf)
{

	memset(buf, 0, sizeof(*buf));

	snprintf(buf->outbuf, sizeof(buf->outbuf), " out=%d/%d/%d",
	    track->outbuf.head, track->outbuf.used, track->outbuf.capacity);
	if (track->freq.filter)
		snprintf(buf->freq, sizeof(buf->freq), " f=%d/%d/%d",
		    track->freq.srcbuf.head,
		    track->freq.srcbuf.used,
		    track->freq.srcbuf.capacity);
	if (track->chmix.filter)
		snprintf(buf->chmix, sizeof(buf->chmix), " m=%d",
		    track->chmix.srcbuf.used);
	if (track->chvol.filter)
		snprintf(buf->chvol, sizeof(buf->chvol), " v=%d",
		    track->chvol.srcbuf.used);
	if (track->codec.filter)
		snprintf(buf->codec, sizeof(buf->codec), " e=%d",
		    track->codec.srcbuf.used);
	snprintf(buf->usrbuf, sizeof(buf->usrbuf), " usr=%d/%d/H%d",
	    track->usrbuf.head, track->usrbuf.used, track->usrbuf_usedhigh);
}

#define TRACE(fmt, ...)		audio_trace(__func__, fmt, ## __VA_ARGS__)
#define TRACET(t, fmt, ...)	audio_tracet(__func__, t, fmt, ## __VA_ARGS__)
#define TRACEF(f, fmt, ...)	audio_tracef(__func__, f, fmt, ## __VA_ARGS__)
#else
#define TRACE(fmt, ...)		/**/
#define TRACET(t, fmt, ...)	/**/
#define TRACEF(f, fmt, ...)	/**/
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

static int audiomatch(device_t, cfdata_t, void *);
static void audioattach(device_t, device_t, void *);
static int audiodetach(device_t, int);
static int audioactivate(device_t, enum devact);
// spkr* at audio? のため?
//static void audiochilddet(device_t, device_t);
//static int audiorescan(device_t, const char *, const int *);

static int audio_modcmd(modcmd_t, void *);

#ifdef AUDIO_PM_IDLE
static void audio_idle(void *);
static void audio_activity(device_t, devactive_t);
#endif

static bool audio_suspend(device_t dv, const pmf_qual_t *);
static bool audio_resume(device_t dv, const pmf_qual_t *);
static void audio_volume_down(device_t);
static void audio_volume_up(device_t);
static void audio_volume_toggle(device_t);

static void audio_mixer_capture(struct audio_softc *);
static void audio_mixer_restore(struct audio_softc *);

static void audio_softintr_rd(void *);
static void audio_softintr_wr(void *);

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
static int audio_close(struct audio_softc *, int, audio_file_t *);
static int audio_read(struct audio_softc *, struct uio *, int, audio_file_t *);
static int audio_write(struct audio_softc *, struct uio *, int, audio_file_t *);
static void audio_file_clear(struct audio_softc *, audio_file_t *);
static int audio_ioctl(dev_t, struct audio_softc *, u_long, void *, int,
	struct lwp *, audio_file_t *);
static int audio_poll(struct audio_softc *, int, struct lwp *, audio_file_t *);
static int audio_kqfilter(struct audio_softc *, audio_file_t *, struct knote *);
static int audio_mmap(struct audio_softc *, off_t *, size_t, int, int *, int *,
	struct uvm_object **, int *, audio_file_t *);

static int audioctl_open(dev_t, struct audio_softc *, int, int, struct lwp *,
	struct file **);

static void audio_pintr(void *);
static void audio_rintr(void *);

static int audio_query_devinfo(struct audio_softc *, mixer_devinfo_t *);

static inline int audio_track_readablebytes(const audio_track_t *);
static int audio_file_setinfo(struct audio_softc *, audio_file_t *,
	const struct audio_info *);
static int audio_file_setinfo_check(audio_format2_t *,
	const struct audio_prinfo *);
static int audio_file_setinfo_set(audio_track_t *, audio_format2_t *, int);
static void audio_track_setinfo_water(audio_track_t *,
	const struct audio_info *);
static int audio_hw_setinfo(struct audio_softc *, const struct audio_info *,
	struct audio_info *);
static int audio_hw_set_params(struct audio_softc *, int, audio_format2_t *,
	audio_format2_t *);
static int audiogetinfo(struct audio_softc *, struct audio_info *, int,
	audio_file_t *);
#if 0 /* not used */
static int audio_getenc(struct audio_softc *, struct audio_encoding *);
#endif
static int audio_get_props(struct audio_softc *);
static bool audio_can_playback(struct audio_softc *);
static bool audio_can_capture(struct audio_softc *);
static int audio_check_params(struct audio_params *);
static int audio_check_params2(audio_format2_t *);
static int audio_mixerpair_init(struct audio_softc *sc, int,
	audio_format2_t *, audio_format2_t *);
static int audio_select_freq(const struct audio_format *);
static int audio_hw_probe(struct audio_softc *, int, int *,
	audio_format2_t *, audio_format2_t *);
static int audio_hw_probe_fmt(struct audio_softc *, audio_format2_t *, int);
static int audio_hw_probe_by_format(struct audio_softc *, audio_format2_t *,
	int);
static int audio_hw_probe_by_encoding(struct audio_softc *, audio_format2_t *,
	int);
static int audio_set_format(struct audio_softc *, audio_format_spec_t *);
static int audio_sysctl_volume(SYSCTLFN_PROTO);
static void audio_format2_tostr(char *, size_t, const audio_format2_t *);
#ifdef AUDIO_DEBUG
static void audio_print_format2(const char *, const audio_format2_t *);
#endif

static void *audio_realloc(void *, size_t);
static int audio_realloc_usrbuf(audio_track_t *, int);
static void audio_free_usrbuf(audio_track_t *);

static int audio_track_init(struct audio_softc *, audio_track_t **, int);
static void audio_track_destroy(audio_track_t *);
static audio_filter_t audio_track_get_codec(const audio_format2_t *,
	const audio_format2_t *);
static int audio_track_set_format(audio_track_t *, audio_format2_t *);
static void audio_track_play(audio_track_t *);
static int audio_track_drain(struct audio_softc *, audio_track_t *);
static void audio_track_record(audio_track_t *);
static void audio_track_clear(struct audio_softc *, audio_track_t *);

static int audio_mixer_init(struct audio_softc *, int, const audio_format2_t *);
static void audio_mixer_destroy(struct audio_softc *, audio_trackmixer_t *);
static void audio_pmixer_start(struct audio_softc *, bool);
static void audio_pmixer_process(struct audio_softc *, bool);
static int  audio_pmixer_mixall(struct audio_softc *, bool);
static int  audio_pmixer_mix_track(audio_trackmixer_t *, audio_track_t *,
	int, int);
static void audio_pmixer_output(struct audio_softc *);
static int  audio_pmixer_halt(struct audio_softc *);
static void audio_rmixer_start(struct audio_softc *);
static void audio_rmixer_process(struct audio_softc *);
static void audio_rmixer_input(struct audio_softc *);
static int  audio_rmixer_halt(struct audio_softc *);

#ifdef OLD_FILTER
static void stream_filter_list_append(stream_filter_list_t *,
	stream_filter_factory_t, const audio_params_t *);
static void stream_filter_list_prepend(stream_filter_list_t *,
	stream_filter_factory_t, const audio_params_t *);
static void stream_filter_list_set(stream_filter_list_t *, int,
	stream_filter_factory_t, const audio_params_t *);
#endif

static void mixer_init(struct audio_softc *);
static int mixer_open(dev_t, struct audio_softc *, int, int, struct lwp *,
	struct file **);
static int mixer_close(struct audio_softc *, int, audio_file_t *);
static int mixer_ioctl(struct audio_softc *, u_long, void *, int, struct lwp *);
static void mixer_remove(struct audio_softc *);
static void mixer_signal(struct audio_softc *);

static int au_portof(struct audio_softc *, char *, int);

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
static int au_set_monitor_gain(struct audio_softc *, int);
static int au_get_monitor_gain(struct audio_softc *);
static int audio_get_port(struct audio_softc *, mixer_ctrl_t *);
static int audio_set_port(struct audio_softc *, mixer_ctrl_t *);

static inline struct audio_params
format2_to_params(const audio_format2_t *f2)
{
	audio_params_t p;

	/* Although audio_format2 and audio_params are compatible. */
	p.sample_rate = f2->sample_rate;
	p.channels = f2->channels;
	p.encoding = f2->encoding;
	p.validbits = f2->precision;
	p.precision = f2->stride;
	return p;
}

static inline audio_format2_t
params_to_format2(const struct audio_params *p)
{
	audio_format2_t f2;

	/* Although audio_format2 and audio_params are compatible. */
	f2.sample_rate = p->sample_rate;
	f2.channels    = p->channels;
	f2.encoding    = p->encoding;
	f2.precision   = p->validbits;
	f2.stride      = p->precision;
	return f2;
}

/* Return true if this track is a playback track. */
static inline bool
audio_track_is_playback(const audio_track_t *track)
{

	return ((track->mode & AUMODE_PLAY) != 0);
}

/* Return true if this track is a recording track. */
static inline bool
audio_track_is_record(const audio_track_t *track)
{

	return ((track->mode & AUMODE_RECORD) != 0);
}

// not used anymore
// どうするかね
#if 0
// ユーザランドで使用される 0..255 ボリュームを、トラック内の内部表現である
// 0..256 ボリュームに変換します。
static inline u_int
audio_volume_to_inner(u_int v)
{

	return v < 127 ? v : v + 1;
}

// トラック内の内部表現である 0..256 ボリュームを、外部で使用される 0..255
// ボリュームに変換します。
static inline u_int
audio_volume_to_outer(u_int v)
{

	return v < 127 ? v : v - 1;
}
#endif // 0

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
	AudioEmpeg_l1_system,
	AudioEmpeg_l2_stream,
	AudioEmpeg_l2_packets,
	AudioEmpeg_l2_system,
	AudioEac3,
};

// エンコーディング名を返します。
// ほぼデバッグ用なのでローカルバッファを返す場合があることに注意。
const char *
audio_encoding_name(int encoding)
{
	static char buf[16];

	if (0 <= encoding && encoding < __arraycount(encoding_names)) {
		return encoding_names[encoding];
	} else {
		snprintf(buf, sizeof(buf), "enc=%d", encoding);
		return buf;
	}
}

/*
 * Supported encodings used by AUDIO_GETENC.
 * index and flags are set by code.
 * XXX is there any needs for SLINEAR_OE:>=16/ULINEAR_OE:>=16 ?
 */
static const audio_encoding_t audio_encodings[] = {
	{ 0, AudioEmulaw,	AUDIO_ENCODING_ULAW,		8,  0 },
	{ 0, AudioEslinear,	AUDIO_ENCODING_SLINEAR,		8,  0 },
	{ 0, AudioEulinear,	AUDIO_ENCODING_ULINEAR,		8,  0 },
	{ 0, AudioEslinear_le,	AUDIO_ENCODING_SLINEAR_LE,	16, 0 },
	{ 0, AudioEulinear_le,	AUDIO_ENCODING_ULINEAR_LE,	16, 0 },
	{ 0, AudioEslinear_be,	AUDIO_ENCODING_SLINEAR_BE,	16, 0 },
	{ 0, AudioEulinear_be,	AUDIO_ENCODING_ULINEAR_BE,	16, 0 },
#if defined(AUDIO_SUPPORT_LINEAR24)
	{ 0, AudioEslinear_le,	AUDIO_ENCODING_SLINEAR_LE,	24, 0 },
	{ 0, AudioEulinear_le,	AUDIO_ENCODING_ULINEAR_LE,	24, 0 },
	{ 0, AudioEslinear_be,	AUDIO_ENCODING_SLINEAR_BE,	24, 0 },
	{ 0, AudioEulinear_be,	AUDIO_ENCODING_ULINEAR_BE,	24, 0 },
#endif
	{ 0, AudioEslinear_le,	AUDIO_ENCODING_SLINEAR_LE,	32, 0 },
	{ 0, AudioEulinear_le,	AUDIO_ENCODING_ULINEAR_LE,	32, 0 },
	{ 0, AudioEslinear_be,	AUDIO_ENCODING_SLINEAR_BE,	32, 0 },
	{ 0, AudioEulinear_be,	AUDIO_ENCODING_ULINEAR_BE,	32, 0 },
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
static char audio_buildinfo[256];

// ビルドオプションを文字列にします。(開発用)
// テスト用なので解放してません。
static void
make_buildinfo(void)
{
	int n;

	if (audio_buildinfo[0] != '\0')
		return;

	n = 0;
	n += snprintf(audio_buildinfo, sizeof(audio_buildinfo),
	    "AUDIO_BLK_MS=%d", AUDIO_BLK_MS);
	n += snprintf(audio_buildinfo + n, sizeof(audio_buildinfo) - n,
	    ", NBLKOUT=%d", NBLKOUT);
#if defined(AUDIO_HW_SINGLE_BUFFER)
	n += snprintf(audio_buildinfo + n, sizeof(audio_buildinfo) - n,
	    ", HW_SINGLE_BUFFER");
#endif
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
	audio_format2_t phwfmt;
	audio_format2_t rhwfmt;
	const struct sysctlnode *node;
	void *hdlp;
	bool is_indep;
	int mode;
	int props;
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
	if (hw_if->query_format == NULL ||
	    (hw_if->set_params == NULL && hw_if->set_format == NULL) ||
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

	if ((props & AUDIO_PROP_FULLDUPLEX) != 0) {
		aprint_normal(": full duplex");
	} else {
		aprint_normal(": half duplex");
	}

	is_indep = (props & AUDIO_PROP_INDEPENDENT);
	mode = 0;
	if ((props & AUDIO_PROP_PLAYBACK)) {
		mode |= AUMODE_PLAY;
		aprint_normal(", playback");
	}
	if ((props & AUDIO_PROP_CAPTURE)) {
		mode |= AUMODE_RECORD;
		aprint_normal(", capture");
	}
	if ((props & AUDIO_PROP_MMAP) != 0)
		aprint_normal(", mmap");
	if (is_indep)
		aprint_normal(", independent");

	aprint_naive("\n");
	aprint_normal("\n");

	// play と capture が両方立ってない状況はたぶん起きない

	/* probe hw params */
	memset(&phwfmt, 0, sizeof(phwfmt));
	memset(&rhwfmt, 0, sizeof(rhwfmt));
	mutex_enter(sc->sc_lock);
	if (audio_hw_probe(sc, is_indep, &mode, &phwfmt, &rhwfmt) != 0) {
		mutex_exit(sc->sc_lock);
		goto bad;
	}
	if (mode == 0) {
		mutex_exit(sc->sc_lock);
		goto bad;
	}
	/* init hardware */
	error = audio_hw_set_params(sc, mode, &phwfmt, &rhwfmt);
	if (error) {
		mutex_exit(sc->sc_lock);
		goto bad;
	}

	/* init track mixer */
	mode = audio_mixerpair_init(sc, mode, &phwfmt, &rhwfmt);
	mutex_exit(sc->sc_lock);
	if (mode == 0)
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
		// XXX adhoc debug info (should be removed)
		// デバッグ用のビルドオプション表示
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

#if defined(AUDIO_DEBUG_MLOG)
	audio_mlog_init();
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

/*
 * Initialize hardware mixer.
 * This function is called from audioattach().
 */
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

	/* device is not initialized */
	if (sc->hw_if == NULL)
		return 0;

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

#if defined(AUDIO_DEBUG_MLOG)
	audio_mlog_free();
#endif

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
static int
audio_waitio(struct audio_softc *sc, audio_track_t *track)
{
	// XXX 自分がいなくなることを想定する必要があるのかどうか
	int error;

	KASSERT(track);
	KASSERT(mutex_owned(sc->sc_lock));

	TRACET(track, "wait");
	/* Wait for pending I/O to complete. */
	error = cv_wait_sig(&track->outchan, sc->sc_lock);
	if (sc->sc_dying)
		error = EIO;
	TRACET(track, "error=%d", error);
	return error;
}


// XXX audiobell はここなのか?

/* Exported interfaces for audiobell. */
int
audiobellopen(dev_t dev, int flags, int ifmt, struct lwp *l, struct file **fp)
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
		error = audio_open(dev, sc, flags, ifmt, l, &fp);
		break;
	case AUDIOCTL_DEVICE:
		error = audioctl_open(dev, sc, flags, ifmt, l, &fp);
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

	KASSERT(fp->f_audioctx);
	file = (audio_file_t *)fp->f_audioctx;
	dev = file->dev;

	if ((error = audio_enter(dev, &sc)) != 0)
		return error;

	device_active(sc->dev, DVA_SYSTEM);
	switch (AUDIODEV(dev)) {
	case SOUND_DEVICE:
	case AUDIO_DEVICE:
		error = audio_close(sc, fp->f_flag, file);
		break;
	case AUDIOCTL_DEVICE:
		error = 0;
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

	KASSERT(fp->f_audioctx);
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

	KASSERT(fp->f_audioctx);
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

	KASSERT(fp->f_audioctx);
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
	audio_file_t *file;

	KASSERT(fp->f_audioctx);
	file = (audio_file_t *)fp->f_audioctx;

	memset(st, 0, sizeof(*st));

	st->st_dev = file->dev;
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

	KASSERT(fp->f_audioctx);
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

	KASSERT(fp->f_audioctx);
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

static int
audiommap(struct file *fp, off_t *offp, size_t len, int prot, int *flagsp,
	int *advicep, struct uvm_object **uobjp, int *maxprotp)
{
	struct audio_softc *sc;
	audio_file_t *file;
	dev_t dev;
	int error;

	KASSERT(fp->f_audioctx);
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
	audio_ring_t *hwbuf;
	int fd;
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->hw_if == NULL)
		return ENXIO;

#if AUDIO_DEBUG > 2
	TRACE("start flags=0x%x po=%d ro=%d",
	    flags, sc->sc_popens, sc->sc_ropens);
#else
	DPRINTF(1, "%s\n", __func__);
#endif

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

		/* On playback side, one softint is assigned to one track. */
		af->ptrack->sih_wr = softint_establish(
		    SOFTINT_SERIAL | SOFTINT_MPSAFE,
		    audio_softintr_wr, af);
		if (af->ptrack->sih_wr == NULL)
			goto bad2;
	}
	if ((af->mode & AUMODE_RECORD) != 0) {
		error = audio_track_init(sc, &af->rtrack, AUMODE_RECORD);
		if (error)
			goto bad2;

		/* On record side, one softint is shared by all tracks. */
	}

	/*
	 * Multiplex device: /dev/audio (MU-Law) and /dev/sound (linear)
	 * The /dev/audio is always (re)set to 8-bit MU-Law mono
	 * For the other devices, you get what they were last set to.
	 */
	AUDIO_INITINFO(&ai);
	if (ISDEVAUDIO(dev)) {
		// /dev/audio は毎回初期化
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
	} else {
		// sc_[pr]params, sc_[pr]pause が現在の /dev/sound の設定値
		ai.play.sample_rate   = sc->sc_pparams.sample_rate;
		ai.play.encoding      = sc->sc_pparams.encoding;
		ai.play.channels      = sc->sc_pparams.channels;
		ai.play.precision     = sc->sc_pparams.precision;
		ai.play.pause         = sc->sc_ppause;
		ai.record.sample_rate = sc->sc_rparams.sample_rate;
		ai.record.encoding    = sc->sc_rparams.encoding;
		ai.record.channels    = sc->sc_rparams.channels;
		ai.record.precision   = sc->sc_rparams.precision;
		ai.record.pause       = sc->sc_rpause;
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
			int hwflags;

			// hw_if->open() は録音再生合わせて1本目のオープン時
			// のみ呼び出すので、Full Duplex HW の場合
			// hw_if->open() に渡す flags はこの open の flags に
			// 関係なく常に FREAD | FWRITE でなければならない。
			// 1本目が再生のみだろうと録音のみだろうと、2本目が
			// 同じ方向のオープンとは限らないため。
			// see also: dev/isa/aria.c
			//
			// 一方、Half Duplex HW では当然 FREAD と FWRITE が
			// 同時に立つことはあり得ないので、必ず有効な片方だけ
			// にしなければならない。
			// see also: arch/evbarm/mini2440/audio_mini2440.c

			if ((audio_get_props(sc) & AUDIO_PROP_FULLDUPLEX)) {
				hwflags = FREAD | FWRITE;
			} else {
				// af->mode から flags を再構成。
				// flags はユーザ指定値、それを他ディスクリプタ
				// のオープン状況と擦り合わせた結果が af->mode
				// に入っている (see audio_file_setinfo())。
				hwflags = 0;
				if ((af->mode & AUMODE_PLAY) != 0)
					hwflags |= FWRITE;
				if ((af->mode & AUMODE_RECORD) != 0)
					hwflags |= FREAD;
			}

			mutex_enter(sc->sc_intr_lock);
			error = sc->hw_if->open(sc->hw_hdl, hwflags);
			mutex_exit(sc->sc_intr_lock);
			if (error)
				goto bad3;
		}

		/* Set speaker mode if half duplex */
		// XXX audio(9) にはハーフで録再を切り替える際に使うと
		// 書いてある気がするんだけど、あと
		// Full dup で録再同時に起きてたらどうするのかとか。
		if (1/*XXX*/) {
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
					goto bad4;
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
			hwbuf = &sc->sc_pmixer->hwbuf;
			mutex_enter(sc->sc_intr_lock);
			error = sc->hw_if->init_output(sc->hw_hdl,
			    hwbuf->mem,
			    hwbuf->capacity *
			    hwbuf->fmt.channels * hwbuf->fmt.stride / NBBY);
			mutex_exit(sc->sc_intr_lock);
			if (error)
				goto bad4;
		}
	}
	if (af->rtrack && sc->sc_ropens == 0) {
		if (sc->hw_if->init_input) {
			hwbuf = &sc->sc_rmixer->hwbuf;
			mutex_enter(sc->sc_intr_lock);
			error = sc->hw_if->init_input(sc->hw_hdl,
			    hwbuf->mem,
			    hwbuf->capacity *
			    hwbuf->fmt.channels * hwbuf->fmt.stride / NBBY);
			mutex_exit(sc->sc_intr_lock);
			if (error)
				goto bad4;
		}
	}

	error = fd_allocfile(&fp, &fd);
	if (error)
		goto bad4;

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
bad4:
	if (sc->sc_popens + sc->sc_ropens == 0) {
		if (sc->hw_if->close) {
			mutex_enter(sc->sc_intr_lock);
			sc->hw_if->close(sc->hw_hdl);
			mutex_exit(sc->sc_intr_lock);
		}
	}
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
audio_close(struct audio_softc *sc, int flags, audio_file_t *file)
{
	audio_track_t *oldtrack;
	int error;

#if AUDIO_DEBUG > 2
	TRACEF(file, "start pid=%d.%d po=%d ro=%d",
	    (int)curproc->p_pid, (int)curlwp->l_lid,
	    sc->sc_popens, sc->sc_ropens);
#else
	DPRINTF(1, "%s\n", __func__);
#endif
	KASSERT(mutex_owned(sc->sc_lock));
	KASSERTMSG(sc->sc_popens + sc->sc_ropens > 0,
	    "sc->sc_popens=%d, sc->sc_ropens=%d",
	    sc->sc_popens, sc->sc_ropens);

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
	    (audio_get_props(sc) & AUDIO_PROP_FULLDUPLEX) == 0 &&
	    sc->hw_if->halt_input == sc->hw_if->halt_output &&
	    sc->sc_ropens == 1) {
		aprint_error_dev(sc->dev,
		    "%s has halt_input == halt_output. Please fix it\n",
		    device_xname(sc->sc_dev));
		// そうは言いつつもとりあえず回避はしておく
		mutex_enter(sc->sc_intr_lock);
		sc->sc_rbusy = false;
		sc->sc_rmixer->hwbuf.head = 0;
		mutex_exit(sc->sc_intr_lock);
	}

	if (file->rtrack) {
		/* Call hw halt_input if this is the last recording track. */
		if (sc->sc_ropens == 1) {
			if (sc->sc_rbusy) {
				DPRINTF(2, "%s halt_input\n", __func__);
				mutex_enter(sc->sc_intr_lock);
				error = audio_rmixer_halt(sc);
				mutex_exit(sc->sc_intr_lock);
				if (error) {
					aprint_error_dev(sc->dev,
					    "halt_input failed with %d\n",
					    error);
				}
			}
		}

		/* Destroy the track. */
		oldtrack = file->rtrack;
		mutex_enter(sc->sc_intr_lock);
		file->rtrack = NULL;
		mutex_exit(sc->sc_intr_lock);
		TRACET(oldtrack, "dropframes=%" PRIu64, oldtrack->dropframes);
		audio_track_destroy(oldtrack);

		KASSERT(sc->sc_ropens > 0);
		sc->sc_ropens--;
	}

	if (file->ptrack) {
		audio_track_drain(sc, file->ptrack);

		/* Call hw halt_output if this is the last playback track. */
		if (sc->sc_popens == 1) {
			if (sc->sc_pbusy) {
				mutex_enter(sc->sc_intr_lock);
				error = audio_pmixer_halt(sc);
				mutex_exit(sc->sc_intr_lock);
				if (error) {
					aprint_error_dev(sc->dev,
					    "halt_output failed with %d\n",
					    error);
				}
			}
		}

		/* Disestablish softint_wr. */
		KASSERT(file->ptrack->sih_wr);
		/* softint_disestablish needs unlock. */
		mutex_exit(sc->sc_lock);
		softint_disestablish(file->ptrack->sih_wr);
		mutex_enter(sc->sc_lock);
		file->ptrack->sih_wr = NULL;

		/* Destroy the track. */
		oldtrack = file->ptrack;
		mutex_enter(sc->sc_intr_lock);
		file->ptrack = NULL;
		mutex_exit(sc->sc_intr_lock);
		TRACET(oldtrack, "dropframes=%" PRIu64, oldtrack->dropframes);
		audio_track_destroy(oldtrack);

		KASSERT(sc->sc_popens > 0);
		sc->sc_popens--;
	}

	/* Call hw close if this is the last track. */
	if (sc->sc_popens + sc->sc_ropens == 0) {
		if (sc->hw_if->close) {
			DPRINTF(2, "%s hw_if close\n", __func__);
			mutex_enter(sc->sc_intr_lock);
			sc->hw_if->close(sc->hw_hdl);
			mutex_exit(sc->sc_intr_lock);
		}

		kauth_cred_free(sc->sc_cred);
	}

	mutex_enter(sc->sc_intr_lock);
	SLIST_REMOVE(&sc->sc_files, file, audio_file, entry);
	mutex_exit(sc->sc_intr_lock);

	TRACE("done");
	return 0;
}

// track の usrbuf の head から len バイトを uiomove します。
// リングバッファの折り返しはしません。
static inline int
audio_read_uiomove(audio_track_t *track, int head, int len, struct uio *uio)
{
	audio_ring_t *usrbuf;
	int error;

	usrbuf = &track->usrbuf;
	error = uiomove((uint8_t *)usrbuf->mem + head, len, uio);
	if (error) {
		TRACET(track, "uiomove(len=%d) failed: %d", len, error);
		return error;
	}
	auring_take(usrbuf, len);
	track->useriobytes += len;
	TRACET(track, "uiomove(len=%d) usrbuf=%d/%d/C%d",
	    len,
	    usrbuf->head, usrbuf->used, usrbuf->capacity);
	return 0;
}

int
audio_read(struct audio_softc *sc, struct uio *uio, int ioflag,
	audio_file_t *file)
{
	audio_track_t *track;
	audio_ring_t *usrbuf;
	audio_ring_t *input;
	int error;

	track = file->rtrack;
	KASSERT(track);
	TRACET(track, "resid=%u", (int)uio->uio_resid);
	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->hw_if == NULL)
		return ENXIO;

	// N8 までは EINVAL だったがこっちのほうがよかろう
	if (track->mmapped)
		return EPERM;

#ifdef AUDIO_PM_IDLE
	if (device_is_active(&sc->dev) || sc->sc_idle)
		device_active(&sc->dev, DVA_SYSTEM);
#endif

	// 自分が O_RDWR でオープンしてあれば read() を発行することは出来るが、
	// HW が half-duplex で自分か他の誰かによって play が選択されていると
	// この read() はエラー。
	if ((file->mode & AUMODE_RECORD) == 0) {
		return EBADF;
	}

	TRACET(track, "resid=%zd", uio->uio_resid);

	usrbuf = &track->usrbuf;
	input = track->input;

	error = 0;
	while (uio->uio_resid > 0 && error == 0) {
		int bytes;

		TRACET(track, "while resid=%zd input=%d/%d/%d usrbuf=%d/%d/H%d",
		    uio->uio_resid,
		    input->head, input->used, input->capacity,
		    usrbuf->head, usrbuf->used, track->usrbuf_usedhigh);

		mutex_enter(sc->sc_intr_lock);
		if (input->used == 0 && usrbuf->used == 0) {
			// バッファが空ならここで待機
			mutex_exit(sc->sc_intr_lock);

			audio_rmixer_start(sc);
			if (ioflag & IO_NDELAY)
				return EWOULDBLOCK;

			error = audio_waitio(sc, track);
			if (error)
				return error;
			continue;
		}

		track->in_use = true;
		mutex_exit(sc->sc_intr_lock);
		audio_track_record(track);
		mutex_enter(sc->sc_intr_lock);
		track->in_use = false;
		mutex_exit(sc->sc_intr_lock);

		bytes = min(usrbuf->used, uio->uio_resid);
		int head = usrbuf->head;
		if (head + bytes <= usrbuf->capacity) {
			error = audio_read_uiomove(track, head, bytes, uio);
			if (error)
				break;
		} else {
			int bytes1;
			int bytes2;

			bytes1 = usrbuf->capacity - head;
			error = audio_read_uiomove(track, head, bytes1, uio);
			if (error)
				break;

			bytes2 = bytes - bytes1;
			error = audio_read_uiomove(track, 0, bytes2, uio);
			if (error)
				break;
		}
	}

	return error;
}


// この file の再生 track と録音 track を即座に空にします。
// ミキサーおよび HW には関与しません。呼ばれるのは以下の2か所から:
// o audio_ioctl AUDIO_FLUSH で一切合切クリアする時
// o audiosetinfo でパラメータを変更する必要がある時
//
// 元々 audio_clear() で、録音再生をその場で停止して hw halt_input/output も
// 呼んでいた (呼ぶ必要があったのかどうかは分からない)。
/*
 * Clear file's playback and/or record track buffer immediately.
 */
static void
audio_file_clear(struct audio_softc *sc, audio_file_t *file)
{

	if (file->ptrack)
		audio_track_clear(sc, file->ptrack);
	if (file->rtrack)
		audio_track_clear(sc, file->rtrack);
}

// track の usrbuf に tail から len バイトを uiomove します。
// リングバッファの折り返しはしません。
static inline int
audio_write_uiomove(audio_track_t *track, int tail, int len, struct uio *uio)
{
	audio_ring_t *usrbuf;
	int error;

	usrbuf = &track->usrbuf;
	error = uiomove((uint8_t *)usrbuf->mem + tail, len, uio);
	if (error) {
		TRACET(track, "uiomove(len=%d) failed: %d", len, error);
		return error;
	}
	auring_push(usrbuf, len);
	track->useriobytes += len;
	TRACET(track, "uiomove(len=%d) usrbuf=%d/%d/C%d",
	    len,
	    usrbuf->head, usrbuf->used, usrbuf->capacity);
	return 0;
}

int
audio_write(struct audio_softc *sc, struct uio *uio, int ioflag,
	audio_file_t *file)
{
	audio_track_t *track;
	audio_ring_t *usrbuf;
	audio_ring_t *outbuf;
	int error;
	int framesize;
	int count;

	track = file->ptrack;
	KASSERT(track);
	TRACET(track, "begin pid=%d.%d ioflag=0x%x",
	    (int)curproc->p_pid, (int)curlwp->l_lid, ioflag);

	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->hw_if == NULL)
		return ENXIO;

	// N8 までは EINVAL だったがこっちのほうがよかろう
	if (track->mmapped)
		return EPERM;

	if (uio->uio_resid == 0) {
		track->eofcounter++;
		return 0;
	}

#ifdef AUDIO_PM_IDLE
	if (device_is_active(&sc->dev) || sc->sc_idle)
		device_active(&sc->dev, DVA_SYSTEM);
#endif

	usrbuf = &track->usrbuf;
	outbuf = &track->outbuf;
	TRACET(track, "resid=%zd", uio->uio_resid);

	track->pstate = AUDIO_STATE_RUNNING;
	error = 0;
	while (uio->uio_resid > 0 && error == 0) {
		int bytes;
		int tail;

		TRACET(track, "while resid=%zd usrbuf=%d/%d/H%d",
		    uio->uio_resid,
		    usrbuf->head, usrbuf->used, track->usrbuf_usedhigh);

		// usrbuf が一杯ならここで待つ
		while (usrbuf->used >= track->usrbuf_usedhigh) {
			if ((ioflag & IO_NDELAY)) {
				error = EWOULDBLOCK;
				goto abort;
			}
			TRACET(track, "sleep usrbuf=%d/H%d",
			    usrbuf->used, track->usrbuf_usedhigh);
			error = audio_waitio(sc, track);
			if (error)
				goto abort;
		}

		/* Write to the conversion stage as much as possible. */
		// usrbuf にコピー
		bytes = min(track->usrbuf_usedhigh - usrbuf->used,
		    uio->uio_resid);
		tail = auring_tail(usrbuf);
		if (tail + bytes <= usrbuf->capacity) {
			error = audio_write_uiomove(track, tail, bytes, uio);
			if (error)
				break;
		} else {
			int bytes1;
			int bytes2;

			bytes1 = usrbuf->capacity - tail;
			error = audio_write_uiomove(track, tail, bytes1, uio);
			if (error)
				break;

			bytes2 = bytes - bytes1;
			error = audio_write_uiomove(track, 0, bytes2, uio);
			if (error)
				break;
		}

		// 前回落としたフレーム分を回復運転のため取り去る
		// write されるバイトバッファがフレーム境界に揃っている
		// 保証はないので、usrbuf にコピーした後でなければならない
		// XXX 出来れば wait する前、あるいは uiomove する前に
		//     drop を計算してしまったほうがいいはず。
		if ((track->mode & AUMODE_PLAY_ALL) == 0 &&
		    track->playdrop > 0) {
			framesize = frametobyte(&track->inputfmt, 1);
			count = MIN(usrbuf->used / framesize, track->playdrop);
			auring_take(usrbuf, count * framesize);
			track->playdrop -= count;
			TRACET(track, "drop %d -> usr=%d/%d/H%d",
			    count * framesize,
			    usrbuf->head, usrbuf->used, track->usrbuf_usedhigh);
		}

		if (track->is_pause)
			continue;

		mutex_enter(sc->sc_intr_lock);
		while (sc->sc_pbusy == 0 &&
		    usrbuf->used >= track->usrbuf_blksize &&
		    outbuf->used < track->mixer->frames_per_block * 2) {
			track->in_use = true;
			mutex_exit(sc->sc_intr_lock);
			audio_track_play(track);
			mutex_enter(sc->sc_intr_lock);
			track->in_use = false;
		}
		mutex_exit(sc->sc_intr_lock);

		// XXX うーんなんだこれ
		if (sc->sc_pbusy == 0 &&
		    track->outbuf.used >= track->mixer->frames_per_block * 2) {
			bool force = ((track->mode & AUMODE_PLAY_ALL) == 0);
			audio_pmixer_start(sc, force);
		}
	}

abort:
	TRACET(track, "done error=%d", error);
	return error;
}


// たぶん audio_ioctl はハードウェアレイヤの変更は行わないはずなので
// これ実行中に同時に他の操作が来ても大丈夫?
// AUDIO_FLUSH .. トラックまでしか触らない。
// AUDIO_DRAIN .. トラックまでしか触らない。
// AUDIO_SETFD .. これもたぶんトラックだけで済ませたほうがいい。
int
audio_ioctl(dev_t dev, struct audio_softc *sc, u_long cmd, void *addr, int flag,
	struct lwp *l, audio_file_t *file)
{
	struct audio_offset *ao;
	audio_track_t *track;
	audio_trackmixer_t *mixer;
	audio_encoding_t *ae;
	audio_format_query_t *query;
	audio_format_spec_t *spec;
	u_int stamp;
	u_int offs;
	int fd;
	int index;
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

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
		" AUDIO_QUERYFORMAT",	// 38
		" AUDIO_GETFORMAT",	// 39
		" AUDIO_SETFORMAT",	// 40
	};
	int nameidx = (cmd & 0xff);
	const char *ioctlname = "";
	if (21 <= nameidx && nameidx <=37)
		ioctlname = ioctlnames[nameidx - 21];
	DPRINTF(2, "audio_ioctl(%lu,'%c',%lu)%s pid=%d.%d\n",
		 IOCPARM_LEN(cmd), (char)IOCGROUP(cmd), cmd&0xff, ioctlname,
	    (int)curproc->p_pid, (int)l->l_lid);
#endif
	if (sc->hw_if == NULL)
		return ENXIO;
	error = 0;
	switch (cmd) {
	case FIONBIO:
		/* All handled in the upper FS layer. */
		break;

	case FIONREAD:
		// 入力バッファにあるバイト数
		// XXX 動作未確認
		if (file->rtrack) {
			*(int *)addr = file->rtrack->usrbuf.used;
		} else {
			*(int *)addr = 0;
		}
		break;

	case FIOASYNC:
		if (*(int *)addr) {
			file->async_audio = curproc->p_pid;
			DPRINTF(2, "%s: FIOASYNC pid %d\n", __func__,
			    file->async_audio);
		} else {
			file->async_audio = 0;
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
		// 渡すことができなかったフレーム数をユーザランドフォーマット
		// 換算でバイト数にしたもの。厳密に正しいかは分からないが
		// そもそもこのエラーカウント自体がどこで
		// どのように落としたものかは問わないとあるのでこれでいい。
		track = file->rtrack;
		if (track) {
			*(int *)addr = frametobyte(&track->usrbuf.fmt,
			    track->dropframes);
		}
		break;

	case AUDIO_PERROR:
		// ここでカウントしてるのはユーザからの write が1ブロックに
		// 満たずに落としたフレーム数なので、バイト数としては
		// ユーザ指定フォーマット換算となる。
		track = file->ptrack;
		if (track) {
			*(int *)addr = frametobyte(&track->usrbuf.fmt,
			    track->dropframes);
		}
		break;

	/*
	 * Offsets into buffer.
	 */
	case AUDIO_GETIOFFS:
		ao = (struct audio_offset *)addr;
		ao->samples = 0;
		ao->deltablks = 0;
		ao->offset = 0;
		break;

	case AUDIO_GETOOFFS:
		ao = (struct audio_offset *)addr;
		track = file->ptrack;
		if (track == NULL) {
			// track がなければダミーデータで埋めるしかない
			ao->samples = 0;
			ao->deltablks = 0;
			ao->offset = 0;
			break;
		}
		mutex_enter(sc->sc_intr_lock);
		/* figure out where next DMA will start */
		stamp = track->usrbuf_stamp;
		offs = track->usrbuf.head;
		mutex_exit(sc->sc_intr_lock);

		ao->samples = stamp;
		ao->deltablks = (stamp / track->usrbuf_blksize) -
		    (track->usrbuf_stamp_last / track->usrbuf_blksize);
		track->usrbuf_stamp_last = stamp;
		offs = rounddown(offs, track->usrbuf_blksize)
		    + track->usrbuf_blksize;
		if (offs >= track->usrbuf.capacity)
			offs -= track->usrbuf.capacity;
		ao->offset = offs;

		TRACET(track, "GETOOFFS: samples=%u deltablks=%u offset=%u",
		    ao->samples, ao->deltablks, ao->offset);
		break;

	/*
	 * How many bytes will elapse until mike hears the first
	 * sample of what we write next?
	 */
	case AUDIO_WSEEK:
		if (file->ptrack)
			*(u_long *)addr = file->ptrack->usrbuf.used;
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
		error = audio_track_drain(sc, file->ptrack);
		break;

	case AUDIO_GETDEV:
		error = sc->hw_if->getdev(sc->hw_hdl, (audio_device_t *)addr);
		break;

	case AUDIO_GETENC:
		ae = (audio_encoding_t *)addr;
		index = ae->index;
		if (index < 0 || index >= __arraycount(audio_encodings)) {
			error = EINVAL;
			break;
		}
		*ae = audio_encodings[index];
		ae->index = index;
		// 常に EMULATED とする。ここでいう非EMULATED とは無加工で
		// HW に渡せるというような意味合いだと思うので、であれば
		// 例えネイティブフォーマットであったとしてもトラック処理
		// などの加工が入っているという考え方をしてみたが、どうか。
		// 本当にネイティブフォーマットかどうかを判断するのは結構
		// 面倒くさい。
		ae->flags = AUDIO_ENCODINGFLAG_EMULATED;
		break;

	case AUDIO_GETFD:
		// 現在のディスクリプタが full duplex かどうかを返す。
		/* Whether this descriptor (not the hardware) is full duplex. */
		if ((file->mode & (AUMODE_PLAY | AUMODE_RECORD)) ==
		    (AUMODE_PLAY | AUMODE_RECORD)) {
			fd = 1;
		} else {
			fd = 0;
		}
		*(int *)addr = fd;
		break;

	case AUDIO_SETFD:
		// HW が full duplex なら half duplex への変更は認めない。
		// この操作に意味があるとは思えない。
		// HW が half duplex なら full duplex へは変更できない(自明)。
		/*
		 * If the hardware is half duplex, it can not change this
		 * descriptor to full duplex.  It's obvious.
		 * If the hardware is full duplex, it's not permitted to
		 * change this descriptor to half duplex.  It's nonsense
		 * operation, I think.
		 */
		fd = *(int *)addr;
		if ((file->mode & (AUMODE_PLAY | AUMODE_RECORD)) ==
		    (AUMODE_PLAY | AUMODE_RECORD)) {
			if (fd == 0)
				error = ENOTTY;
		} else {
			if (fd)
				error = ENOTTY;
		}
		break;

	case AUDIO_GETPROPS:
		*(int *)addr = audio_get_props(sc);
		break;

	case AUDIO_QUERYFORMAT:
		query = (audio_format_query_t *)addr;
		error = sc->hw_if->query_format(sc->hw_hdl, query);
		break;

	case AUDIO_GETFORMAT:
		spec = (audio_format_spec_t *)addr;
		if (spec->mode == AUMODE_PLAY) {
			mixer = sc->sc_pmixer;
		} else if (spec->mode == AUMODE_RECORD) {
			mixer = sc->sc_rmixer;
		} else {
			error = EINVAL;
			break;
		}
		if (mixer == NULL) {
			error = ENOTTY;
			break;
		}

		// XXX
		// ここのターゲットはハードウェアフォーマットではなく、
		// ミキサーフォーマット。この辺どうしたもんだか。
		spec->encoding    = mixer->track_fmt.encoding;
		spec->precision   = mixer->track_fmt.precision;
		spec->stride      = mixer->track_fmt.stride;
		spec->channels    = mixer->track_fmt.channels;
		spec->sample_rate = mixer->track_fmt.sample_rate;
		break;

	case AUDIO_SETFORMAT:
		error = audio_set_format(sc, (audio_format_spec_t *)addr);
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

// XXX 場所は後で考えなおしたほうがいい
// 録音バッファの読み取り可能バイト数を返します。
/*
 * Returns the number of bytes that can be read from recording buffer.
 */
static inline int
audio_track_readablebytes(const audio_track_t *track)
{
	int bytes;

	KASSERT(track);
	KASSERT(track->mode == AUMODE_RECORD);

	// ユーザランドから読み取り可能なデータは本来なら usrbuf だが、
	// 録音したデータは read が発行されるまでは track->input に滞留
	// しているため、これを単位変換したものを加える必要がある。
	// track->input はフレーム単位、usrbuf はバイト単位。
	bytes = track->usrbuf.used +
	    track->input->used * frametobyte(&track->usrbuf.fmt, 1);
	return bytes;
}

int
audio_poll(struct audio_softc *sc, int events, struct lwp *l,
	audio_file_t *file)
{
	audio_track_t *track;
	int revents;
	bool in_is_valid;
	bool out_is_valid;

	KASSERT(mutex_owned(sc->sc_lock));

#if AUDIO_DEBUG > 2
#define POLLEV_BITMAP "\177\020" \
	    "b\10WRBAND\0" \
	    "b\7RDBAND\0" "b\6RDNORM\0" "b\5NVAL\0" "b\4HUP\0" \
	    "b\3ERR\0" "b\2OUT\0" "b\1PRI\0" "b\0IN\0"
	char evbuf[64];
	snprintb(evbuf, sizeof(evbuf), POLLEV_BITMAP, events);
	TRACEF(file, "pid=%d.%d events=%s",
	    (int)curproc->p_pid, (int)l->l_lid, evbuf);
#else
	DPRINTF(2, "%s: events=0x%x mode=%d\n", __func__, events, file->mode);
#endif

	revents = 0;
	in_is_valid = false;
	out_is_valid = false;
	if (events & (POLLIN | POLLRDNORM)) {
		track = file->rtrack;
		if (track) {
			int used;
			in_is_valid = true;
			used = audio_track_readablebytes(track);
			if (used > track->usrbuf_usedlow)
				revents |= events & (POLLIN | POLLRDNORM);
		}
	}
	if (events & (POLLOUT | POLLWRNORM)) {
		track = file->ptrack;
		if (track) {
			out_is_valid = true;
			if (track->usrbuf.used <= track->usrbuf_usedlow)
				revents |= events & (POLLOUT | POLLWRNORM);
		}
	}

	if (revents == 0) {
		if (in_is_valid) {
			TRACEF(file, "selrecord rsel");
			selrecord(l, &sc->sc_rsel);
		}
		if (out_is_valid) {
			TRACEF(file, "selrecord wsel");
			selrecord(l, &sc->sc_wsel);
		}
	}

#if AUDIO_DEBUG > 2
	snprintb(evbuf, sizeof(evbuf), POLLEV_BITMAP, revents);
	TRACEF(file, "revents=%s", evbuf);
#else
	DPRINTF(2, "%s: revents=0x%x\n", __func__, revents);
#endif
	return revents;
}

static void
filt_audiordetach(struct knote *kn)
{
	struct audio_softc *sc;
	audio_file_t *file;

	file = kn->kn_hook;
	sc = file->sc;

	SLIST_REMOVE(&sc->sc_rsel.sel_klist, kn, knote, kn_selnext);
}

static int
filt_audioread(struct knote *kn, long hint)
{
	audio_file_t *file;
	audio_track_t *track;

	file = kn->kn_hook;
	track = file->rtrack;

	// XXX READ 可能な時しかここ来ないのかな?

	// kn_data は read 可能なバイト数っぽい?
	kn->kn_data = 0;
	if (track) {
		kn->kn_data = audio_track_readablebytes(track);
	}

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

	TRACE("kn=%p", kn);
	file = kn->kn_hook;
	sc = file->sc;

	SLIST_REMOVE(&sc->sc_wsel.sel_klist, kn, knote, kn_selnext);
}

static int
filt_audiowrite(struct knote *kn, long hint)
{
	audio_file_t *file;
	audio_track_t *track;

	file = kn->kn_hook;
	track = file->ptrack;

	// XXX WRITE 可能な時しかここ来ないのかな?

	// XXX 仕様がよくわからんけど、
	// 再生バッファの空きバイト数を調べるだけじゃいかんのか?

	kn->kn_data = 0;
	if (track) {
		kn->kn_data = track->usrbuf_usedhigh - track->usrbuf.used;
	}

	TRACE("kn=%p data=%d", kn, (int)kn->kn_data);
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

	KASSERT(mutex_owned(sc->sc_lock));
	TRACEF(file, "kn=%p kn_filter=%x", kn, (int)kn->kn_filter);

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

	SLIST_INSERT_HEAD(klist, kn, kn_selnext);

	return 0;
}

int
audio_mmap(struct audio_softc *sc, off_t *offp, size_t len, int prot,
	int *flagsp, int *advicep, struct uvm_object **uobjp, int *maxprotp,
	audio_file_t *file)
{
	audio_track_t *track;
	vsize_t vsize;

	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->hw_if == NULL)
		return ENXIO;

	DPRINTF(2, "%s: off=%lld, prot=%d\n",
	    __func__, (long long)(*offp), prot);

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
	// read/write どちらのバッファをマップするか決めるのに prot を使う
	// この案はうまくいかない。
	// VM システムは(少なくとも)以下の2つの点で壊れている。
	// 1) メモリを VM_PROT_WRITE でマップして書き込むと SIGSEGV になる。
	//    なので再生バッファには VM_PROT_READ|VM_PROT_WRITE を使わないと
	//    いけない。
	// 2) mmap を VM_PROT_READ|VM_PROT_WRITE でコールしたとしても
	//    VM_PROT_READ のみで audio_mmap が呼び出される場合がある?
	// なので再生バッファだけをマップすることにする。
	if (prot == (VM_PROT_READ|VM_PROT_WRITE) ||
	    prot == VM_PROT_WRITE)
		track = file->ptrack;
	else if (prot == VM_PROT_READ)
		track = file->rtrack;
	else
		return EINVAL;
#else
	track = file->ptrack;
#endif
	if (track == NULL)
		return EACCES;

	vsize = roundup2(MAX(track->usrbuf.capacity, PAGE_SIZE), PAGE_SIZE);
	if (len > vsize)
		return EOVERFLOW;
	if (*offp > (uint)(vsize - len))
		return EOVERFLOW;

	// 二重 mmap したらどうなるか
	if (!track->mmapped) {
		track->mmapped = true;

		// XXX ここで元は audio_fill_silence
		if (!track->is_pause)
			audio_pmixer_start(sc, true);
		/* XXX mmapping record buffer is not supported */
	}

	/* get ringbuffer */
	*uobjp = track->uobj;

	/* Acquire a reference for the mmap.  munmap will release.*/
	uao_reference(*uobjp);
	*maxprotp = prot;
	*advicep = UVM_ADV_RANDOM;
	*flagsp = MAP_SHARED;
	return 0;
}

/*
 * /dev/audioctl has to be able to open at any time without interference with
 * any /dev/audio or /dev/sound.
 */
static int
audioctl_open(dev_t dev, struct audio_softc *sc, int flags, int ifmt,
	struct lwp *l, struct file **nfp)
{
	struct file *fp;
	audio_file_t *af;
	int fd;
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	if (sc->hw_if == NULL)
		return ENXIO;

#if AUDIO_DEBUG > 2
	TRACE("");
#else
	DPRINTF(1, "%s\n", __func__);
#endif

	error = fd_allocfile(&fp, &fd);
	if (error)
		return error;

	af = kmem_zalloc(sizeof(audio_file_t), KM_SLEEP);
	af->sc = sc;
	af->dev = dev;

	// sc_files にはつながなくていいはず

	error = fd_clone(fp, fd, flags, &audio_fileops, af);
	KASSERT(error == EMOVEFD);

	*nfp = fp;
	return error;
}

// ここに audiostart{pr} があった

// audio_pint_silence いるかどうかは今後

/*
 * Reallocate 'memblock' with specified 'bytes' if 'bytes' > 0.
 * Or free 'memblock' and return NULL if 'byte' is zero.
 */
static void *
audio_realloc(void *memblock, size_t bytes)
{

	if (memblock != NULL) {
		if (bytes != 0) {
			return kern_realloc(memblock, bytes, M_NOWAIT);
		} else {
			kern_free(memblock);
			return NULL;
		}
	} else {
		if (bytes != 0) {
			return kern_malloc(bytes, M_NOWAIT);
		} else {
			return NULL;
		}
	}
}

/*
 * Free 'mem' if available, and initialize the pointer.
 * For this reason, this is implemented as macro.
 */
#define audio_free(mem)	do {	\
	if (mem != NULL) {	\
		kern_free(mem);	\
		mem = NULL;	\
	}	\
} while (0)

// usrbuf を newbufsize で確保し直します。
// usrbuf は mmap されるためこちらを使用してください。
// usrbuf.capacity を更新する前に呼び出してください。
// メモリが確保できれば track->mem、track->capacity をセットし 0 を返します。
// 確保できなければ track->mem、track->capacity をクリアし errno を返します。
/*
 * (Re)allocate usrbuf with 'newbufsize' bytes.
 * Use this function for usrbuf because only usrbuf may be mmapped.
 * If successful, it updates track->usrbuf.mem, track->usrbuf.capacity and
 * returns 0.  Otherwise, it clears track->usrbuf.mem, track->usrbuf.capacity
 * and returns errno.
 * It must be called before updating usrbuf.capacity.
 */
static int
audio_realloc_usrbuf(audio_track_t *track, int newbufsize)
{
	vaddr_t vstart;
	vsize_t oldvsize;
	vsize_t newvsize;
	int error;

	KASSERT(newbufsize > 0);

	/* Get a nonzero multiple of PAGE_SIZE */
	newvsize = roundup2(MAX(newbufsize, PAGE_SIZE), PAGE_SIZE);

	if (track->usrbuf.mem != NULL) {
		oldvsize = roundup2(MAX(track->usrbuf.capacity, PAGE_SIZE),
		    PAGE_SIZE);
		if (oldvsize == newvsize) {
			track->usrbuf.capacity = newbufsize;
			return 0;
		}
		vstart = (vaddr_t)track->usrbuf.mem;
		uvm_unmap(kernel_map, vstart, vstart + oldvsize);
		/* uvm_unmap also detach uobj */
		track->uobj = NULL;		/* paranoia */
		track->usrbuf.mem = NULL;
	}

	/* Create a uvm anonymous object */
	track->uobj = uao_create(newvsize, 0);

	/* Map it into the kernel virtual address space */
	vstart = 0;
	error = uvm_map(kernel_map, &vstart, newvsize, track->uobj, 0, 0,
	    UVM_MAPFLAG(UVM_PROT_RW, UVM_PROT_RW, UVM_INH_NONE,
	    UVM_ADV_RANDOM, 0));
	if (error) {
		DPRINTF(1, "%s: uvm_map failed\n", __func__);
		uao_detach(track->uobj);	/* release reference */
		goto abort;
	}

	error = uvm_map_pageable(kernel_map, vstart, vstart + newvsize,
	    false, 0);
	if (error) {
		DPRINTF(1, "%s: uvm_map_pageable failed\n", __func__);
		uvm_unmap(kernel_map, vstart, vstart + newvsize);
		/* uvm_unmap also detach uobj */
		goto abort;
	}

	track->usrbuf.mem = (void *)vstart;
	track->usrbuf.capacity = newbufsize;
	memset(track->usrbuf.mem, 0, newvsize);
	return 0;

	/* failure */
abort:
	track->uobj = NULL;		/* paranoia */
	track->usrbuf.mem = NULL;
	track->usrbuf.capacity = 0;
	return error;
}

/*
 * Free usrbuf (if available).
 */
static void
audio_free_usrbuf(audio_track_t *track)
{
	vaddr_t vstart;
	vsize_t vsize;

	vstart = (vaddr_t)track->usrbuf.mem;
	vsize = roundup2(MAX(track->usrbuf.capacity, PAGE_SIZE), PAGE_SIZE);
	if (track->usrbuf.mem != NULL) {
		/*
		 * Unmap the kernel mapping.  uvm_unmap releases the
		 * reference to the uvm object, and this should be the
		 * last virtual mapping of the uvm object, so no need
		 * to explicitly release (`detach') the object.
		 */
		uvm_unmap(kernel_map, vstart, vstart + vsize);

		track->uobj = NULL;
		track->usrbuf.mem = NULL;
		track->usrbuf.capacity = 0;
	}
}

/*
 * This filter changes the volume for each channel.
 * arg->context points track->ch_volume[].
 */
static void
audio_track_chvol(audio_filter_arg_t *arg)
{
	int16_t *ch_volume;
	const aint_t *s;
	aint_t *d;
	u_int i;
	u_int ch;
	u_int channels;

	DIAGNOSTIC_filter_arg(arg);
	KASSERT(arg->srcfmt->channels == arg->dstfmt->channels);
	KASSERT(arg->context != NULL);
	KASSERT(arg->srcfmt->channels <= AUDIO_MAX_CHANNELS);

	s = arg->src;
	d = arg->dst;
	ch_volume = arg->context;

	channels = arg->srcfmt->channels;
	for (i = 0; i < arg->count; i++) {
		for (ch = 0; ch < channels; ch++) {
			aint2_t val;
			val = *s++;
#if defined(AUDIO_USE_C_IMPLEMENTATION_DEFINED_BEHAVIOR) && defined(__GNUC__)
			val = val * ch_volume[ch] >> 8;
#else
			val = val * ch_volume[ch] / 256;
#endif
			*d++ = (aint_t)val;
		}
	}
}

/*
 * This filter performs conversion from stereo (or more channels) to mono.
 */
static void
audio_track_chmix_mixLR(audio_filter_arg_t *arg)
{
	// L と R、2つのサンプルの算術平均を取る。
	//  mixed = (L + R) / 2;
	// のように先に加算をしてしまうと整数オーバーフローが起きうる。
	// aint2_t にキャストしてから演算すれば問題ないが、このために
	// そこまでするかという気はする。
	//
	// そこで L と R を先に割っておいてから足す。
	//  mixed = (L / 2) + (R / 2);  // (1)
	// この式で例えば L = 1, R = 1 の場合数学的には答えは1 になってほしいが
	// 先に切り捨てが起きるため答えは 0 となり、誤差は全域で最大
	// (aint_t が 16bit の場合) 1/65536 (かな?)。
	//
	// ところで C で負数の除算は 0 方向への丸めと定義されているため、
	// アセンブラの算術右シフトだけでは実現できず (算術右シフトはマイナス
	// 無限大方向への丸めになる)、実行時に値が負数だったら 1補正する
	// みたいな処理が(たぶん大抵)書いてある。少なくとも gcc 4.8 とかには。
	// ただしここの目的は (たぶんこの後スピーカーから出て人間の耳に届く)
	// 音声波形をなるべく高速に処理したいところなので、その誤差 1/65536
	// を許容して高速化したい。
	// ところが C で負数の右シフトは処理系定義となっている (不定動作では
	// ない)。GCC では負数の右シフトは算術右シフトと定義してあるようなので
	// https://gcc.gnu.org/onlinedocs/gcc-5.5.0/gcc/ (section 4.5)
	// もし許されるなら使いたいところ。
	//  mixed = (L >> 1) + (R >> 1); // (2)
	// この場合の誤差は負領域のみで最大 (aint_t が 16bit の場合)
	// 1/65536 (かな?)。
	//
	//	amd64 [times/msec]	x68k [times/sec]
	// (1)	 78.8			176.4
	// (2)	150.1			245.8

	const aint_t *s;
	aint_t *d;
	u_int i;

	DIAGNOSTIC_filter_arg(arg);

	s = arg->src;
	d = arg->dst;

	for (i = 0; i < arg->count; i++) {
#if defined(AUDIO_USE_C_IMPLEMENTATION_DEFINED_BEHAVIOR) && defined(__GNUC__)
		*d++ = (s[0] >> 1) + (s[1] >> 1);
#else
		*d++ = (s[0] / 2) + (s[1] / 2);
#endif
		s += arg->srcfmt->channels;
	}
}

/*
 * This filter performs conversion from mono to stereo (or more channels).
 */
static void
audio_track_chmix_dupLR(audio_filter_arg_t *arg)
{
	const aint_t *s;
	aint_t *d;
	u_int i;
	u_int ch;
	u_int dstchannels;

	DIAGNOSTIC_filter_arg(arg);

	s = arg->src;
	d = arg->dst;
	dstchannels = arg->dstfmt->channels;

	for (i = 0; i < arg->count; i++) {
		d[0] = s[0];
		d[1] = s[0];
		s++;
		d += dstchannels;
	}
	if (dstchannels > 2) {
		d = arg->dst;
		for (i = 0; i < arg->count; i++) {
			for (ch = 2; ch < dstchannels; ch++) {
				d[ch] = 0;
			}
			d += dstchannels;
		}
	}
}

/*
 * This filter shrinks M channels into N channels.
 * Extra channels are discarded.
 */
static void
audio_track_chmix_shrink(audio_filter_arg_t *arg)
{
	const aint_t *s;
	aint_t *d;
	u_int i;
	u_int ch;

	DIAGNOSTIC_filter_arg(arg);

	s = arg->src;
	d = arg->dst;

	for (i = 0; i < arg->count; i++) {
		for (ch = 0; ch < arg->dstfmt->channels; ch++) {
			*d++ = s[ch];
		}
		s += arg->srcfmt->channels;
	}
}

/*
 * This filter expands M channels into N channels.
 * Silence is inserted for missing channels.
 */
static void
audio_track_chmix_expand(audio_filter_arg_t *arg)
{
	const aint_t *s;
	aint_t *d;
	u_int i;
	u_int ch;
	u_int srcchannels;
	u_int dstchannels;

	DIAGNOSTIC_filter_arg(arg);

	s = arg->src;
	d = arg->dst;

	srcchannels = arg->srcfmt->channels;
	dstchannels = arg->dstfmt->channels;
	for (i = 0; i < arg->count; i++) {
		for (ch = 0; ch < srcchannels; ch++) {
			*d++ = *s++;
		}
		for (; ch < dstchannels; ch++) {
			*d++ = 0;
		}
	}
}

// 周波数変換は入出力周波数の比 (srcfreq / dstfreq) で計算を行う。
// そのまま分数で計算するのがシンプルだが、ここでは除算回数を減らす
// ため dstfreq を 65536 とした時の src/dst 比を用いる。
// なおこのアイデアは S44PLAY.X から拝借したもの。
//  http://stdkmd.com/kohx3/
//
// 例えば入力 24kHz を 48kHz に変換する場合は src/dst = 32768/65536 と
// なり、この分子 32768 が track->freq_step である。
// 原理としては出力1サンプルごとに変数(ここでは t)に freq_step を
// 加算していき、これが 65536 以上になるごとに入力を行って、その間を
// 補間する。
//
// 入出力周波数の組み合わせによっては freq_step が整数にならない場合も
// 当然ある。例えば入力 8kHz を 48kHz に変換する場合
//  freq_step = 8000 / 48000 * 65536 = 10922.6666…
// となる。
// この場合出力1サンプルあたり理論値よりも 0.6666 ずつカウントが少なく
// なるわけなので、これをブロックごとに補正する。
// 1ブロックの時間 AUDIO_BLK_MS が標準の 40msec であれば、出力周波数
// 48kHz に対する1ブロックの出力サンプル数は
//  dstcount = 48000[Hz] * 0.04[sec] = 1920
// より 1920個なので、補正値は
//  freq_leap = 0.6666… * 1920 = 1280
// となる。つまり 8kHz を 48kHz に変換する場合、1920 出力サンプルごとに
// t にこの 1280 を足すことで周波数変換誤差は出なくなる。
//
// さらに freq_leap が整数にならないような入出力周波数の組み合わせも
// もちろんありうるが、日常使う程度の組み合わせではほぼ発生しないと
// 思うし、また発生したとしてもその誤差は 10^-6 以下でありこれは水晶
// 振動子の誤差程度かそれ以下であるので、用途に対しては十分許容できる
// と思う。

// AUDIO_DEBUG=1、AUDIO_ASSERT なしで main.c による計測。
//
//        44->48    8->48   48->44    48->8	[times/msec]
// amd64    70.9    113.4    177.8    984.8	Pentium DC E5400/2.7GHz
// x68k    0.048    0.065    0.129    0.688	68030/30MHz

// 周波数変換(アップサンプリング)をします。線形補間です。
/*
 * This filter performs frequency conversion (up sampling).
 * It uses linear interpolation.
 */
static void
audio_track_freq_up(audio_filter_arg_t *arg)
{
	audio_track_t *track;
	audio_ring_t *src;
	audio_ring_t *dst;
	const aint_t *s;
	aint_t *d;
	aint_t prev[AUDIO_MAX_CHANNELS];
	aint_t curr[AUDIO_MAX_CHANNELS];
	aint_t grad[AUDIO_MAX_CHANNELS];
	u_int i;
	u_int t;
	u_int step;
	u_int channels;
	u_int ch;
	int srcused;

	track = arg->context;
	KASSERT(track);
	src = &track->freq.srcbuf;
	dst = track->freq.dst;
	DIAGNOSTIC_ring(dst);
	DIAGNOSTIC_ring(src);
	KASSERT(src->used > 0);
	KASSERT(src->fmt.channels == dst->fmt.channels);
	KASSERT(src->head % track->mixer->frames_per_block == 0);

	s = arg->src;
	d = arg->dst;

	// 補間はブロック単位での処理がしやすいように入力を1サンプルずらして(?)
	// 補間を行なっている。このため厳密には位相が 1/dstfreq 分だけ遅れる
	// ことになるが、これによる観測可能な影響があるとは思えない。
	/*
	 * In order to faciliate interpolation for each block, slide (delay)
	 * input by one sample.  As a result, strictly speaking, the output
	 * phase is delayed by 1/dstfreq.  However, I believe there is no
	 * observable impact.
	 *
	 * Example)
	 * srcfreq:dstfreq = 1:3
	 *
	 *  A - -
	 *  |
	 *  |
	 *  |     B - -
	 *  +-----+-----> input timeframe
	 *  0     1
	 *
	 *  0     1
	 *  +-----+-----> input timeframe
	 *  |     A
	 *  |   x   x
	 *  | x       x
	 *  x          (B)
	 *  +-+-+-+-+-+-> output timeframe
	 *  0 1 2 3 4 5
	 */

	// 前回の最終サンプル
	channels = src->fmt.channels;
	for (ch = 0; ch < channels; ch++) {
		prev[ch] = track->freq_prev[ch];
		curr[ch] = track->freq_curr[ch];
		grad[ch] = curr[ch] - prev[ch];
	}

	step = track->freq_step;
	t = track->freq_current;
//#define FREQ_DEBUG
#if defined(FREQ_DEBUG)
#define PRINTF(fmt...)	printf(fmt)
#else
#define PRINTF(fmt...)	/**/
#endif
	srcused = src->used;
	PRINTF("start step=%d leap=%d", step, track->freq_leap);
	PRINTF(" srcused=%d arg->count=%u", src->used, arg->count);
	PRINTF(" prev=%d curr=%d grad=%d", prev[0], curr[0], grad[0]);
	PRINTF(" t=%d\n", t);

	for (i = 0; i < arg->count; i++) {
		PRINTF("i=%d t=%5d", i, t);
		if (t >= 65536) {
			for (ch = 0; ch < channels; ch++) {
				// 前回値
				prev[ch] = curr[ch];
				// 今回値
				curr[ch] = *s++;
				// 傾き
				grad[ch] = curr[ch] - prev[ch];
			}
			PRINTF(" prev=%d s[%d]=%d",
			    prev[0], src->used - srcused, curr[0]);

			// 更新
			t -= 65536;
			srcused--;
			if (srcused < 0) {
				PRINTF(" break\n");
				break;
			}
		}

		for (ch = 0; ch < channels; ch++) {
			*d++ = prev[ch] + (aint2_t)grad[ch] * t / 65536;
#if defined(FREQ_DEBUG)
			if (ch == 0)
				printf(" t=%5d *d=%d", t, d[-1]);
#endif
		}
		t += step;

		PRINTF("\n");
	}
	PRINTF("end prev=%d curr=%d\n", prev[0], curr[0]);

	auring_take(src, src->used);
	auring_push(dst, i);

	// 補正
	t += track->freq_leap;

	track->freq_current = t;
	for (ch = 0; ch < channels; ch++) {
		track->freq_prev[ch] = prev[ch];
		track->freq_curr[ch] = curr[ch];
	}
}

// 周波数変換(ダウンサンプリング)をします。単純間引きです。
/*
 * This filter performs frequency conversion (down sampling).
 * It uses simple thinning.
 */
static void
audio_track_freq_down(audio_filter_arg_t *arg)
{
	audio_track_t *track;
	audio_ring_t *src;
	audio_ring_t *dst;
	const aint_t *s0;
	aint_t *d;
	u_int i;
	u_int t;
	u_int step;
	u_int ch;
	u_int channels;

	track = arg->context;
	KASSERT(track);
	src = &track->freq.srcbuf;
	dst = track->freq.dst;

	DIAGNOSTIC_ring(dst);
	DIAGNOSTIC_ring(src);
	KASSERT(src->used > 0);
	KASSERT(src->fmt.channels == dst->fmt.channels);
	KASSERTMSG(src->head % track->mixer->frames_per_block == 0,
	    "src->head=%d fpb=%d",
	    src->head, track->mixer->frames_per_block);

	s0 = arg->src;
	d = arg->dst;
	t = track->freq_current;
	step = track->freq_step;
	channels = dst->fmt.channels;

	for (i = 0; i < arg->count && t / 65536 < src->used; i++) {
		const aint_t *s;
		s = s0 + (t / 65536) * channels;
		for (ch = 0; ch < channels; ch++) {
			*d++ = s[ch];
		}
		t += step;
	}
	t += track->freq_leap;
	auring_take(src, src->used);
	auring_push(dst, i);
	track->freq_current = t % 65536;
}

// トラックを初期化します。
// 初期化できれば 0 を返して *trackp に初期化済みのトラックを格納します。
// 初期化できなければ errno を返し、*trackp は変更しません。
// mode は再生なら AUMODE_PLAY、録音なら AUMODE_RECORD を指定します。
// 単に録音再生のどちら側かだけなので AUMODE_PLAY_ALL は関係ありません。
// trackp には sc_files に繋がっている file 構造体内のポインタを直接
// 指定してはいけません。呼び出し側で一旦受け取って sc_intr_lock を
// とってから繋ぎ変えてください。
/*
 * Initialize the track.
 * If successful, it stores the allocated and initialized track in *trackp
 * and return 0.  Otherwise, it returns errno without modifying *trackp.
 * For mode, specify AUMODE_PLAY for playback, AUMODE_RECORD for record.
 * AUMODE_PLAY_ALL does not matter here.
 * Don't specify the track within the file structure linked from sc->sc_files.
 */
int
audio_track_init(struct audio_softc *sc, audio_track_t **trackp, int mode)
{
	audio_track_t *track;
	audio_format2_t *default_format;
	audio_trackmixer_t *mixer;
	const char *cvname;
	int error;
	static int newid = 0;

	track = kmem_zalloc(sizeof(*track), KM_SLEEP);

	track->id = newid++;
	// ここだけ id が決まってから表示
	TRACET(track, "for %s", mode == AUMODE_PLAY ? "playback" : "recording");

	if (mode == AUMODE_PLAY) {
		cvname = "audiowr";
		default_format = &sc->sc_pparams;
		mixer = sc->sc_pmixer;
	} else {
		cvname = "audiord";
		default_format = &sc->sc_rparams;
		mixer = sc->sc_rmixer;
	}

	track->mixer = mixer;
	track->mode = mode;
	cv_init(&track->outchan, cvname);

	// 固定初期値
	track->volume = 256;
	for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
		track->ch_volume[i] = 256;
	}

	// デフォルトフォーマットでセット
	error = audio_track_set_format(track, default_format);
	if (error) {
		audio_track_destroy(track);
		return error;
	}

	*trackp = track;
	return 0;
}

// track のすべてのリソースと track 自身を解放します。
// sc_files に繋がっている file 構造体内の [pr]track ポインタを直接
// 指定してはいけません。呼び出し側で sc_intr_lock をとった上でリストから
// 外したものを指定してください。
/*
 * Release all resources of track and track itself.
 * track must not be NULL.  Don't specify the track within the file structure
 * linked from sc->sc_files.
 */
static void
audio_track_destroy(audio_track_t *track)
{

	// 関数仕様を track は NULL 許容にしてもいいけど、これを呼ぶところは
	// たいてい track が NULL でないと分かっていて呼んでるはずなので
	// ASSERT のほうがよかろう。
	KASSERT(track);

	audio_free_usrbuf(track);
	audio_free(track->codec.srcbuf.mem);
	audio_free(track->chvol.srcbuf.mem);
	audio_free(track->chmix.srcbuf.mem);
	audio_free(track->freq.srcbuf.mem);
	audio_free(track->outbuf.mem);
	cv_destroy(&track->outchan);

	kmem_free(track, sizeof(*track));
}

// src, dst のフォーマットに応じて変換フィルタを返します。
// src か dst のどちらか一方が internal 形式でなければなりません。
// 変換できない組み合わせの場合は NULL を返します。
/*
 * It returns encoding conversion filter according to src and dst format.
 * If it is not a convertible pair, it returns NULL.  Either src or dst
 * must be internal format.
 */
static audio_filter_t
audio_track_get_codec(const audio_format2_t *src, const audio_format2_t *dst)
{

	if (audio_format2_is_internal(src)) {
		if (dst->encoding == AUDIO_ENCODING_ULAW) {
			return audio_internal_to_mulaw;
		} else if (audio_format2_is_linear(dst)) {
			switch (dst->stride) {
			case 8:
				return audio_internal_to_linear8;
			case 16:
				return audio_internal_to_linear16;
#if defined(AUDIO_SUPPORT_LINEAR24)
			case 24:
				return audio_internal_to_linear24;
#endif
			case 32:
				return audio_internal_to_linear32;
			default:
				DPRINTF(1, "%s: unsupported %s stride %d\n",
				    __func__, "dst", dst->stride);
				goto abort;
			}
		}
	} else if (audio_format2_is_internal(dst)) {
		if (src->encoding == AUDIO_ENCODING_ULAW) {
			return audio_mulaw_to_internal;
		} else if (audio_format2_is_linear(src)) {
			switch (src->stride) {
			case 8:
				return audio_linear8_to_internal;
			case 16:
				return audio_linear16_to_internal;
#if defined(AUDIO_SUPPORT_LINEAR24)
			case 24:
				return audio_linear24_to_internal;
#endif
			case 32:
				return audio_linear32_to_internal;
			default:
				DPRINTF(1, "%s: unsupported %s stride %d\n",
				    __func__, "src", src->stride);
				goto abort;
			}
		}
	}

	DPRINTF(1, "unsupported encoding\n");
abort:
#if defined(AUDIO_DEBUG)
	{
		char buf[100];
		audio_format2_tostr(buf, sizeof(buf), src);
		printf("%s: src %s\n", __func__, buf);
		audio_format2_tostr(buf, sizeof(buf), dst);
		printf("%s: dst %s\n", __func__, buf);
	}
#endif
	return NULL;
}

// track の codec ステージを必要に応じて初期化します。
// 成功すれば、codec ステージが必要なら初期化した上で、いずれにしても
// 更新された last_dst を last_dstp に格納して、0 を返します。
// 失敗すれば last_dstp は更新せずに errno を返します。
/*
 * Initialize the codec stage of this track as necessary.
 * If successful, it initializes the codec stage as necessary, stores updated
 * last_dst in *last_dstp in any case, and returns 0.
 * Otherwise, it returns errno without modifying *last_dstp.
 */
static int
audio_track_init_codec(audio_track_t *track, audio_ring_t **last_dstp)
{
	audio_ring_t *last_dst;
	audio_ring_t *srcbuf;
	audio_format2_t *srcfmt;
	audio_format2_t *dstfmt;
	audio_filter_arg_t *arg;
	u_int len;
	int error;

	KASSERT(track);

	last_dst = *last_dstp;
	dstfmt = &last_dst->fmt;
	srcfmt = &track->inputfmt;
	srcbuf = &track->codec.srcbuf;
	error = 0;

	if (srcfmt->encoding != dstfmt->encoding
	 || srcfmt->precision != dstfmt->precision
	 || srcfmt->stride != dstfmt->stride) {
		track->codec.dst = last_dst;

		srcbuf->fmt = *dstfmt;
		srcbuf->fmt.encoding = srcfmt->encoding;
		srcbuf->fmt.precision = srcfmt->precision;
		srcbuf->fmt.stride = srcfmt->stride;

		track->codec.filter = audio_track_get_codec(&srcbuf->fmt,
		    dstfmt);
		if (track->codec.filter == NULL) {
			DPRINTF(1, "%s: get_codec_filter failed\n", __func__);
			error = EINVAL;
			goto abort;
		}

		srcbuf->head = 0;
		srcbuf->used = 0;
		srcbuf->capacity = frame_per_block(track->mixer, &srcbuf->fmt);
		len = auring_bytelen(srcbuf);
		srcbuf->mem = audio_realloc(srcbuf->mem, len);
		if (srcbuf->mem == NULL) {
			DPRINTF(1, "%s: malloc(%d) failed\n", __func__, len);
			error = ENOMEM;
			goto abort;
		}

		arg = &track->codec.arg;
		arg->srcfmt = &srcbuf->fmt;
		arg->dstfmt = dstfmt;
		arg->context = NULL;

		*last_dstp = srcbuf;
		return 0;
	}

abort:
	track->codec.filter = NULL;
	audio_free(srcbuf->mem);
	return error;
}

// track の chvol ステージを必要に応じて初期化します。
// 成功すれば、chvol ステージが必要なら初期化した上で、いずれにしても
// 更新された last_dst を last_dstp に格納して、0 を返します。
// 失敗すれば last_dstp は更新せずに errno を返します。
/*
 * Initialize the chvol stage of this track as necessary.
 * If successful, it initializes the chvol stage as necessary, stores updated
 * last_dst in *last_dstp in any case, and returns 0.
 * Otherwise, it returns errno without modifying *last_dstp.
 */
static int
audio_track_init_chvol(audio_track_t *track, audio_ring_t **last_dstp)
{
	audio_ring_t *last_dst;
	audio_ring_t *srcbuf;
	audio_format2_t *srcfmt;
	audio_format2_t *dstfmt;
	audio_filter_arg_t *arg;
	u_int len;
	int error;

	KASSERT(track);

	last_dst = *last_dstp;
	dstfmt = &last_dst->fmt;
	srcfmt = &track->inputfmt;
	srcbuf = &track->chvol.srcbuf;
	error = 0;

	/* Check whether channel volume conversion is necessary. */
	bool use_chvol = false;
	for (int ch = 0; ch < srcfmt->channels; ch++) {
		if (track->ch_volume[ch] != 256) {
			use_chvol = true;
			break;
		}
	}

	if (use_chvol == true) {
		track->chvol.dst = last_dst;
		track->chvol.filter = audio_track_chvol;

		srcbuf->fmt = *dstfmt;
		/* no format conversion occurs */

		srcbuf->head = 0;
		srcbuf->used = 0;
		srcbuf->capacity = frame_per_block(track->mixer, &srcbuf->fmt);
		len = auring_bytelen(srcbuf);
		srcbuf->mem = audio_realloc(srcbuf->mem, len);
		if (srcbuf->mem == NULL) {
			DPRINTF(1, "%s: malloc(%d) failed\n", __func__, len);
			error = ENOMEM;
			goto abort;
		}

		arg = &track->chvol.arg;
		arg->srcfmt = &srcbuf->fmt;
		arg->dstfmt = dstfmt;
		arg->context = track->ch_volume;

		*last_dstp = srcbuf;
		return 0;
	}

abort:
	track->chvol.filter = NULL;
	audio_free(srcbuf->mem);
	return error;
}

// track の chmix ステージを必要に応じて初期化します。
// 成功すれば、chmix ステージが必要なら初期化した上で、いずれにしても
// 更新された last_dst を last_dstp に格納して、0 を返します。
// 失敗すれば last_dstp は更新せずに errno を返します。
/*
 * Initialize the chmix stage of this track as necessary.
 * If successful, it initializes the chmix stage as necessary, stores updated
 * last_dst in *last_dstp in any case, and returns 0.
 * Otherwise, it returns errno without modifying *last_dstp.
 */
static int
audio_track_init_chmix(audio_track_t *track, audio_ring_t **last_dstp)
{
	audio_ring_t *last_dst;
	audio_ring_t *srcbuf;
	audio_format2_t *srcfmt;
	audio_format2_t *dstfmt;
	audio_filter_arg_t *arg;
	u_int srcch;
	u_int dstch;
	u_int len;
	int error;

	KASSERT(track);

	last_dst = *last_dstp;
	dstfmt = &last_dst->fmt;
	srcfmt = &track->inputfmt;
	srcbuf = &track->chmix.srcbuf;
	error = 0;

	srcch = srcfmt->channels;
	dstch = dstfmt->channels;
	if (srcch != dstch) {
		track->chmix.dst = last_dst;

		if (srcch >= 2 && dstch == 1) {
			track->chmix.filter = audio_track_chmix_mixLR;
		} else if (srcch == 1 && dstch >= 2) {
			track->chmix.filter = audio_track_chmix_dupLR;
		} else if (srcch > dstch) {
			track->chmix.filter = audio_track_chmix_shrink;
		} else {
			track->chmix.filter = audio_track_chmix_expand;
		}

		srcbuf->fmt = *dstfmt;
		srcbuf->fmt.channels = srcch;

		srcbuf->head = 0;
		srcbuf->used = 0;
		// バッファサイズは計算で決められるはずだけど。とりあえず。
		srcbuf->capacity = frame_per_block(track->mixer, &srcbuf->fmt);
		len = auring_bytelen(srcbuf);
		srcbuf->mem = audio_realloc(srcbuf->mem, len);
		if (srcbuf->mem == NULL) {
			DPRINTF(1, "%s: malloc(%d) failed\n", __func__, len);
			error = ENOMEM;
			goto abort;
		}

		arg = &track->chmix.arg;
		arg->srcfmt = &srcbuf->fmt;
		arg->dstfmt = dstfmt;
		arg->context = NULL;

		*last_dstp = srcbuf;
		return 0;
	}

abort:
	track->chmix.filter = NULL;
	audio_free(srcbuf->mem);
	return error;
}

// track の freq ステージを必要に応じて初期化します。
// 成功すれば、freq ステージが必要なら初期化した上で、いずれにしても
// 更新された last_dst を last_dstp に格納して、0 を返します。
// 失敗すれば last_dstp は更新せずに errno を返します。
/*
 * Initialize the freq stage of this track as necessary.
 * If successful, it initializes the freq stage as necessary, stores updated
 * last_dst in *last_dstp in any case, and returns 0.
 * Otherwise, it returns errno without modifying *last_dstp.
 */
static int
audio_track_init_freq(audio_track_t *track, audio_ring_t **last_dstp)
{
	audio_ring_t *last_dst;
	audio_ring_t *srcbuf;
	audio_format2_t *srcfmt;
	audio_format2_t *dstfmt;
	audio_filter_arg_t *arg;
	uint32_t srcfreq;
	uint32_t dstfreq;
	u_int dst_capacity;
	u_int mod;
	u_int len;
	int error;

	KASSERT(track);

	last_dst = *last_dstp;
	dstfmt = &last_dst->fmt;
	srcfmt = &track->inputfmt;
	srcbuf = &track->freq.srcbuf;
	error = 0;

	srcfreq = srcfmt->sample_rate;
	dstfreq = dstfmt->sample_rate;
	if (srcfreq != dstfreq) {
		track->freq.dst = last_dst;

		memset(track->freq_prev, 0, sizeof(track->freq_prev));
		memset(track->freq_curr, 0, sizeof(track->freq_curr));

		// freq_step は dstfreq を 65536 とした時の src/dst 比
		track->freq_step = (uint64_t)srcfreq * 65536 / dstfreq;

		// freq_leap は1ブロックごとの freq_step の補正値
		// を四捨五入したもの。
		dst_capacity = frame_per_block(track->mixer, dstfmt);
		mod = (uint64_t)srcfreq * 65536 % dstfreq;
		track->freq_leap = (mod * dst_capacity + dstfreq / 2) / dstfreq;

		if (track->freq_step < 65536) {
			track->freq.filter = audio_track_freq_up;
			// 初回に繰り上がりを起こすため0ではなく 65536 で初期化
			track->freq_current = 65536;
		} else {
			track->freq.filter = audio_track_freq_down;
			// こっちは 0 からでいい
			track->freq_current = 0;
		}

		srcbuf->fmt = *dstfmt;
		srcbuf->fmt.sample_rate = srcfreq;

		srcbuf->head = 0;
		srcbuf->used = 0;
		srcbuf->capacity = frame_per_block(track->mixer, &srcbuf->fmt);
		len = auring_bytelen(srcbuf);
		srcbuf->mem = audio_realloc(srcbuf->mem, len);
		if (srcbuf->mem == NULL) {
			DPRINTF(1, "%s: malloc(%d) failed\n", __func__, len);
			error = ENOMEM;
			goto abort;
		}

		arg = &track->freq.arg;
		arg->srcfmt = &srcbuf->fmt;
		arg->dstfmt = dstfmt;/*&last_dst->fmt;*/
		arg->context = track;

		*last_dstp = srcbuf;
		return 0;
	}

abort:
	track->freq.filter = NULL;
	audio_free(srcbuf->mem);
	return error;
}

/*
 * When playing back: (e.g. if codec and freq stage are valid)
 *
 *               write
 *                | uiomove
 *                v
 *  usrbuf      [...............]  byte ring buffer (mmap-able)
 *                | memcpy
 *                v
 *  codec.srcbuf[....]             1 block (ring) buffer   <-- stage input
 *       .dst ----+
 *                | convert
 *                v
 *  freq.srcbuf [....]             1 block (ring) buffer
 *      .dst  ----+
 *                | convert
 *                v
 *  outbuf      [...............]  NBLKOUT blocks ring buffer
 *
 *
 * When recording:
 *
 *  freq.srcbuf [...............]  NBLKOUT blocks ring buffer <-- stage input
 *      .dst  ----+
 *                | convert
 *                v
 *  codec.srcbuf[.....]            1 block (ring) buffer
 *       .dst ----+
 *                | convert
 *                v
 *  outbuf      [.....]            1 block (ring) buffer
 *                | memcpy
 *                v
 *  usrbuf      [...............]  byte ring buffer (mmap-able *)
 *                | uiomove
 *                v
 *               read
 *
 *    *: recoding usrbuf is also mmap-able due to symmetry with playback
 *       but for now it will not be mmapped.
 */

// トラックのユーザランド側フォーマットを設定します。
// 変換用内部バッファは一度破棄されます。
// 成功すれば 0、失敗すれば errno を返します。
// uvm 関連のルーチンが非 intr_lock 状態であることを要求するため、必ず
// intr_lock でない状態で呼び出してください。
// ただし outbuf を解放・再取得する可能性があるため、track が sc_files
// 上にある場合 (audio_file_setinfo_set() から呼ばれる時) は、in_use を
// セットしてから呼び出してください。
/*
 * Set the userland format of this track.
 * It will release and reallocate all internal conversion buffers.
 * It returns 0 if successful.  Otherwise it returns errno with clearing all
 * internal buffers.
 * It must be called without intr_lock since uvm_* routines require non
 * intr_lock state.  It must be called with setting track->in_use if the
 * track is within the file structure linked from sc->sc_files (as called
 * from audio_file_setinfo_set), since it may release and reallocate outpubuf.
 */
static int
audio_track_set_format(audio_track_t *track, audio_format2_t *usrfmt)
{
	u_int newbufsize;
	u_int oldblksize;
	u_int len;
	int error;

	KASSERT(track);

	// 入力値チェック
	error = audio_check_params2(usrfmt);
	if (error)
		return error;

	// ユーザランド側バッファ
	// 大きさは (ブロックサイズ * AUMINNOBLK) か 64KB の大きいほうにする。
	// ただし usrbuf は基本この fmt を参照せずに、バイトバッファとして扱う。
	track->usrbuf.fmt = *usrfmt;

	// 参考: 1ブロック(40msec)は
	// mulaw/8kHz/1ch で 320 byte    = 204 blocks/64KB
	// s16/48kHz/2ch  で 7680 byte   = 8 blocks/64KB
	// s16/48kHz/8ch  で 30720 byte  = 90KB/3blocks
	// s16/96kHz/8ch  で 61440 byte  = 180KB/3blocks
	// s32/192kHz/8ch で 245760 byte = 720KB/3blocks
	//
	// ちなみに N7 は 64KB を if->round_buffersize() したものとしている。
	// ブロックサイズが 64KB を越えるケースはどうなるか未調査。
	/*
	 * For example,
	 * 1) If usrbuf_blksize = 7056 (s16/44.1k/2ch) and PAGE_SIZE = 8192,
	 *     newbufsize = rounddown(65536 / 7056) = 63504
	 *     newvsize = roundup2(63504, PAGE_SIZE) = 65536
	 *    Therefore it maps 8 * 8K pages and usrbuf->capacity = 63504.
	 *
	 * 2) If usrbuf_blksize = 7680 (s16/48k/2ch) and PAGE_SIZE = 4096,
	 *     newbufsize = rounddown(65536 / 7680) = 61440
	 *     newvsize = roundup2(61440, PAGE_SIZE) = 61440 (= 15 pages)
	 *    Therefore it maps 15 * 4K pages and usrbuf->capacity = 61440.
	 */
	// XXX 64KB と言っておいて 15ページしかとらないケースがあるの大丈夫かな
	// XXX 初期値の mulaw でも 64KB 取るので、/dev/audio 開いて 48k に設定
	//     すると 16page 確保してすぐ解放して 15page 再確保ってなるけど
	//     どうなん…。
	oldblksize = track->usrbuf_blksize;
	track->usrbuf_blksize = frametobyte(&track->usrbuf.fmt,
	    frame_per_block(track->mixer, &track->usrbuf.fmt));
	track->usrbuf.head = 0;
	track->usrbuf.used = 0;
	newbufsize = MAX(track->usrbuf_blksize * AUMINNOBLK, 65536);
	newbufsize = rounddown(newbufsize, track->usrbuf_blksize);
	error = audio_realloc_usrbuf(track, newbufsize);
	if (error) {
		DPRINTF(1, "%s: malloc usrbuf(%d) failed\n", __func__,
		    newbufsize);
		goto error;
	}

	/* Recalc water mark. */
	if (track->usrbuf_blksize != oldblksize) {
		if (audio_track_is_playback(track)) {
			/* Set high at 100%, low at 75%.  */
			track->usrbuf_usedhigh = track->usrbuf.capacity;
			track->usrbuf_usedlow = track->usrbuf.capacity * 3 / 4;
		} else {
			/* Set high at 100% minus 1block(?), low at 0% */
			track->usrbuf_usedhigh = track->usrbuf.capacity -
			    track->usrbuf_blksize;
			track->usrbuf_usedlow = 0;
		}
	}

	/* Stage buffer */
	audio_ring_t *last_dst = &track->outbuf;
	if (audio_track_is_playback(track)) {
		/* On playback, initialize from the mixer side in order. */
		track->inputfmt = *usrfmt;
		track->outbuf.fmt =  track->mixer->track_fmt;

		if ((error = audio_track_init_freq(track, &last_dst)) != 0)
			goto error;
		if ((error = audio_track_init_chmix(track, &last_dst)) != 0)
			goto error;
		if ((error = audio_track_init_chvol(track, &last_dst)) != 0)
			goto error;
		if ((error = audio_track_init_codec(track, &last_dst)) != 0)
			goto error;
	} else {
		/* On recording, initialize from userland side in order. */
		track->inputfmt = track->mixer->track_fmt;
		track->outbuf.fmt = *usrfmt;

		if ((error = audio_track_init_codec(track, &last_dst)) != 0)
			goto error;
		if ((error = audio_track_init_chvol(track, &last_dst)) != 0)
			goto error;
		if ((error = audio_track_init_chmix(track, &last_dst)) != 0)
			goto error;
		if ((error = audio_track_init_freq(track, &last_dst)) != 0)
			goto error;
	}
#if 0
	/* debug */
	if (track->freq.filter) {
		audio_print_format2("freq src", &track->freq.srcbuf.fmt);
		audio_print_format2("freq dst", &track->freq.dst->fmt);
	}
	if (track->chmix.filter) {
		audio_print_format2("chmix src", &track->chmix.srcbuf.fmt);
		audio_print_format2("chmix dst", &track->chmix.dst->fmt);
	}
	if (track->chvol.filter) {
		audio_print_format2("chvol src", &track->chvol.srcbuf.fmt);
		audio_print_format2("chvol dst", &track->chvol.dst->fmt);
	}
	if (track->codec.filter) {
		audio_print_format2("codec src", &track->codec.srcbuf.fmt);
		audio_print_format2("codec dst", &track->codec.dst->fmt);
	}
#endif

	/* Stage input buffer */
	track->input = last_dst;
	// 録音時は先頭のステージをリングバッファにする
	// XXX もっとましな方法でやったほうがいい
	if (audio_track_is_record(track)) {
		track->input->capacity = NBLKOUT *
		    frame_per_block(track->mixer, &track->input->fmt);
		len = auring_bytelen(track->input);
		track->input->mem = audio_realloc(track->input->mem, len);
		if (track->input->mem == NULL) {
			DPRINTF(1, "%s: malloc input(%d) failed\n", __func__,
			    len);
			error = ENOMEM;
			goto error;
		}
	}

	// 出力フォーマットに従って outbuf を作る
	// 再生側 outbuf は NBLKOUT 分。録音側は1ブロックでいい。
	/*
	 * Output buffer.
	 * On the playback track, its capacity is NBLKOUT blocks.
	 * On the recording track, its capacity is 1 block.
	 */
	track->outbuf.head = 0;
	track->outbuf.used = 0;
	track->outbuf.capacity = frame_per_block(track->mixer,
	    &track->outbuf.fmt);
	if (audio_track_is_playback(track))
		track->outbuf.capacity *= NBLKOUT;
	len = auring_bytelen(&track->outbuf);
	track->outbuf.mem = audio_realloc(track->outbuf.mem, len);
	if (track->outbuf.mem == NULL) {
		DPRINTF(1, "%s: malloc outbuf(%d) failed\n", __func__, len);
		error = ENOMEM;
		goto error;
	}

#if AUDIO_DEBUG > 2
	struct audio_track_debugbuf m;

	memset(&m, 0, sizeof(m));
	snprintf(m.outbuf, sizeof(m.outbuf), " out=%d",
	    track->outbuf.capacity * frametobyte(&track->outbuf.fmt, 1));
	if (track->freq.filter)
		snprintf(m.freq, sizeof(m.freq), " freq=%d",
		    track->freq.srcbuf.capacity *
		    frametobyte(&track->freq.srcbuf.fmt, 1));
	if (track->chmix.filter)
		snprintf(m.chmix, sizeof(m.chmix), " chmix=%d",
		    track->chmix.srcbuf.capacity *
		    frametobyte(&track->chmix.srcbuf.fmt, 1));
	if (track->chvol.filter)
		snprintf(m.chvol, sizeof(m.chvol), " chvol=%d",
		    track->chvol.srcbuf.capacity *
		    frametobyte(&track->chvol.srcbuf.fmt, 1));
	if (track->codec.filter)
		snprintf(m.codec, sizeof(m.codec), " codec=%d",
		    track->codec.srcbuf.capacity *
		    frametobyte(&track->codec.srcbuf.fmt, 1));
	snprintf(m.usrbuf, sizeof(m.usrbuf),
	    " usr=%d", track->usrbuf.capacity);

	if (audio_track_is_playback(track)) {
		TRACET(track, "bufsize%s%s%s%s%s%s",
		    m.outbuf, m.freq, m.chmix, m.chvol, m.codec, m.usrbuf);
	} else {
		TRACET(track, "bufsize%s%s%s%s%s%s",
		    m.freq, m.chmix, m.chvol, m.codec, m.outbuf, m.usrbuf);
	}
#endif
	return 0;

error:
	audio_free_usrbuf(track);
	audio_free(track->codec.srcbuf.mem);
	audio_free(track->chvol.srcbuf.mem);
	audio_free(track->chmix.srcbuf.mem);
	audio_free(track->freq.srcbuf.mem);
	audio_free(track->outbuf.mem);
	return error;
}

// ring が空でなく 1 ブロックに満たない時、1ブロックまで無音を追加します。
// 追加したフレーム数を返します。
/*
 * Fill the silence frames (in the internal format) up to 1 block if the
 * ring is less than 1 block and not empty.
 * It returns the number of appended frames.
 */
static int
audio_append_silence(audio_track_t *track, audio_ring_t *ring)
{
	int fpb;
	int n;

	KASSERT(track);
	KASSERT(audio_format2_is_internal(&ring->fmt));

	// XXX n の計算あれでいいんだろうか。最初から contig_free じゃなくて?
	// XXX memset の長さ frametobyte じゃなくて?

	if (ring->used == 0)
		return 0;

	fpb = frame_per_block(track->mixer, &ring->fmt);
	if (ring->used >= fpb)
		return 0;

	n = (ring->capacity - ring->used) % fpb;

	KASSERT(auring_get_contig_free(ring) >= n);

	memset(auring_tailptr_aint(ring), 0,
	    n * ring->fmt.channels * sizeof(aint_t));
	auring_push(ring, n);
	return n;
}

// どこに書くのがいいか分からんけど、フィルタの仕様みたいなもの。
//
// 概要
//	変換フィルタは以下に分類できます。
//	  1. ユーザランド側フィルタ
//	   1a. 周波数変換フィルタ
//	   1b. それ以外のフィルタ
//	  2. HW フィルタ
//
//	1 のユーザランド側フィルタはすべて audio layer が責任を持っています
//	ので MD ドライバは通常使用することはありません。ただし HW フォーマット
//	が mulaw なデバイスのように、audio layer が持っている
//	mulaw <-> aint_t 変換関数をそのまま利用できる場合にはこれを使用
//	することが可能です。
//
//	audio layer が MD ドライバに受け渡すフォーマットは内部形式と呼ぶもので、
//	slinear_NE、16 bit、HW channels、HW frequency です。HW がこれをそのまま
//	扱えるのなら MD フィルタは不要、encoding/precision を変換する必要が
//	あれば MD フィルタが必要です。MD フィルタは通常 slinear_NE、16bit と
//	HW エンコーディングの変換だけを受け持ちます。
//
//	変換関数(フィルタ)は以下のプロトタイプを持ちます。
//
//	  typedef struct {
//		const void *src;
//		const audio_format2_t *srcfmt;
//		void *dst;
//		const audio_format2_t *dstfmt;
//		int count;
//		void *context;
//	  } audio_filter_arg_t;
//
//	  void filter(audio_filter_arg_t *arg);
//
//	変換に必要なパラメータは arg として渡されます。filter() は arg->src
//	から arg->count 個のフレームを読み込んで変換し arg->dst から
//	arg->count 個のフレームに出力します。arg->src および arg->dst は
//	arg->count フレームの読み書きが連続して行えることを保証しています。
//
//	arg->count 個を全部処理する前の早期終了や途中でのエラー終了は今の所
//	認められていません。また arg->src、arg->dst ポインタの進み具合によって
//	呼び出し元に実際の読み込み数や書き込み数を通知することは出来ませんので
//	arg->src、arg->dst は filter() 側で破壊(変更)しても構いません。
//
//	同様に arg->count も filter() 側で破壊(変更)可能です。
//
//	arg->srcfmt, arg->dstfmt には入出力のフォーマットが記述されています。
//	通常フィルタは自分自身が何から何への変換なのかを知っているので入出力
//	フォーマットをチェックする必要はないでしょう (例えば mulaw から
//	aint_t への変換、など)。一方リニア PCM から aint_t への変換を
//	すべて受け持つフィルタの場合は srcfmt をチェックする必要があるでしょう。
//
//	context はフィルタ自身が自由に使用可能なポインタです。audio layer は
//	この値について一切関与しません。
//
// 周波数変換フィルタ
//	周波数変換フィルタだけ入出力のフレーム数が変化するため、いろいろと
//	特別対応が必要です。まず入出力リングバッファは再生/録音の両方向とも
//	freq.srcbuf、freq.dst なのでそれはもう直接使ってしまうことにします。
//
//	つまり filter() 呼び出し時点で要求出力フレーム数は arg->count フレーム、
//	入力可能なフレーム数は freq.srcbuf.used フレームです。
//
//	変換処理によって消費した入力フレーム数は freq.srcbuf を、出力した
//	フレーム数は freq.dst を filter() 側が更新してください。
//	arg->count は破壊(変更)して構いません。
//

// 変換ステージを実行します。
// stage->filter != NULL の時だけ呼び出してください。
// stage から arg を用意して stage->filter を処理します。
// 周波数変換なら src, dst のカウンタはフィルタ側で進めてください。
// 周波数変換以外なら src, dst のカウンタはここで進めます。
/*
 * Execute the conversion stage.
 * It prepares arg from this stage and executes stage->filter.
 * It must be called only if stage->filter is not NULL.
 *
 * For stages other than frequency conversion, the src and dst counters
 * are incremented here.  On the other hand, for frequency conversion stage,
 * it's the responsibility of the filter side to increment src and dst
 * counters.
 */
static void
audio_apply_stage(audio_track_t *track, audio_stage_t *stage, bool isfreq)
{
	audio_filter_arg_t *arg;
	int srccount;
	int dstcount;
	int count;

	KASSERT(track);
	KASSERT(stage->filter);

	srccount = auring_get_contig_used(&stage->srcbuf);
	dstcount = auring_get_contig_free(stage->dst);

	if (isfreq) {
		KASSERTMSG(srccount > 0, "freq but srccount == %d", srccount);
		count = min(dstcount, track->mixer->frames_per_block);
	} else {
		count = min(srccount, dstcount);
	}

	if (count > 0) {
		arg = &stage->arg;
		arg->src = auring_headptr(&stage->srcbuf);
		arg->dst = auring_tailptr(stage->dst);
		arg->count = count;

		stage->filter(arg);

		if (!isfreq) {
			auring_take(&stage->srcbuf, count);
			auring_push(stage->dst, count);
		}
	}
}

// 再生時の入力データを変換してトラックバッファに投入します。
// usrbuf が空でないことは呼び出し側でチェックしてから呼んでください。
// outbuf に1ブロック以上の空きがあることは呼び出し側でチェックしてから
// 呼んでください。
static void
audio_track_play(audio_track_t *track)
{
	audio_ring_t *usrbuf;
	audio_ring_t *input;
	int count;
	int framesize;	// input の1フレームのバイト数
	int bytes;		// usrbuf から input に転送するバイト数
	u_int dropcount;

	KASSERT(track);
	TRACET(track, "start pstate=%d", track->pstate);

	// この時点で usrbuf は空ではない。
	/* At this point usrbuf should not be empty. */
	KASSERT(track->usrbuf.used > 0);
	// また outbuf に1ブロック以上の空きがある。
	/* Also, outbuf should be available at least one block. */
	count = auring_get_contig_free(&track->outbuf);
	KASSERTMSG(count >= frame_per_block(track->mixer, &track->outbuf.fmt),
	    "count=%d fpb=%d",
	    count, frame_per_block(track->mixer, &track->outbuf.fmt));

	int track_count_0 = track->outbuf.used;

	usrbuf = &track->usrbuf;
	input = track->input;
	dropcount = 0;

	// 入力(usrfmt) に 4bit は来ないので1フレームは必ず1バイト以上ある
	/*
	 * framesize is always 1 byte or more since all formats supported as
	 * usrfmt(=input) have 8bit or more stride.
	 */
	framesize = frametobyte(&input->fmt, 1);
	KASSERT(framesize >= 1);

	// usrbuf の次段(input) も空いてるはず
	/* The next stage of usrbuf (=input) should be available. */
	KASSERT(auring_get_contig_free(input) > 0);

	// usrbuf の最大1ブロックを input へコピー
	// count は usrbuf からコピーするフレーム数。
	// bytes は usrbuf からコピーするバイト数。
	// ただし1フレーム未満のバイトはコピーしない。
	/*
	 * Copy usrbuf up to 1block to input buffer.
	 * count is the number of frames to copy from usrbuf.
	 * bytes is the number of bytes to copy from usrbuf.  However it is
	 * not copied less than 1 frame.
	 */
	count = min(usrbuf->used, track->usrbuf_blksize) / framesize;
	bytes = count * framesize;

	// 今回処理するバイト数(bytes) が1ブロックに満たない場合、
	//  drain AUMODE
	//  ----- ------
	//  no    PLAY  : 溜まっていないのでここで帰る
	//  no    SYNC  : dropframes加算する。  リングバッファリセット。
	//  yes   PLAY  : dropframes加算しない。リングバッファリセット。
	//  yes   SYNC  : dropframes加算しない。リングバッファリセット。
	if (count < track->usrbuf_blksize / framesize) {
		dropcount = track->usrbuf_blksize / framesize - count;

		if (track->pstate != AUDIO_STATE_DRAINING) {
			if ((track->mode & AUMODE_PLAY_ALL)) {
				// PLAY_ALL なら溜まるまで待つ
				/* For PLAY_ALL, wait until filled. */
				TRACET(track, "not enough; return");
				return;
			} else {
				// ここで1ブロックに満たなければ
				// 落ちる(落ちた)ということ
				/* Drop occurred. */

				// playdrop は今落とし中のフレーム数
				// 回復運転のために数えておく必要がある
				/*
				 * playdrop is the number of frames currently
				 * being dropped.  It is necessary to remember
				 * due to recover sync.
				 */
				track->playdrop += dropcount;
				// dropframes は数え上げるだけ。
				// 1回でも落ちれば play.error を立てるため
				/*
				 * dropframes only counts up due to set
				 * play.error if drops even once.
				 */
				track->dropframes += dropcount;
			}
		}
	}

	// stamp はハードウェアで再生したバイト数に相当するので
	// 無音挿入分も入れてここでカウントする。
	// この時点で必ず1ブロック分になってる気がする。
	track->usrbuf_stamp += bytes;

	if (usrbuf->head + bytes < usrbuf->capacity) {
		memcpy((uint8_t *)input->mem + auring_tail(input) * framesize,
		    (uint8_t *)usrbuf->mem + usrbuf->head,
		    bytes);
		auring_push(input, count);
		auring_take(usrbuf, bytes);
	} else {
		int bytes1;
		int bytes2;

		bytes1 = auring_get_contig_used(usrbuf);
		KASSERT(bytes1 % framesize == 0);
		memcpy((uint8_t *)input->mem + auring_tail(input) * framesize,
		    (uint8_t *)usrbuf->mem + usrbuf->head,
		    bytes1);
		auring_push(input, bytes1 / framesize);
		auring_take(usrbuf, bytes1);

		bytes2 = bytes - bytes1;
		memcpy((uint8_t *)input->mem + auring_tail(input) * framesize,
		    (uint8_t *)usrbuf->mem + usrbuf->head,
		    bytes2);
		auring_push(input, bytes2 / framesize);
		auring_take(usrbuf, bytes2);
	}

	// エンコーディング変換
	/* Encoding conversion */
	if (track->codec.filter)
		audio_apply_stage(track, &track->codec, false);

	// チャンネルボリューム
	/* Channel volume */
	if (track->chvol.filter)
		audio_apply_stage(track, &track->chvol, false);

	// チャンネルミキサ
	/* Channel mix */
	if (track->chmix.filter)
		audio_apply_stage(track, &track->chmix, false);

	// 周波数変換
	// 1ブロックごとに誤差補正があるので必ず1ブロックにする
	/* Frequency conversion */
	/*
	 * Since the frequency conversion needs correction for each block,
	 * it rounds up to 1 block.
	 */
	if (track->freq.filter) {
		int n;
		n = audio_append_silence(track, &track->freq.srcbuf);
		if (n > 0) {
			TRACET(track,
			    "freq.srcbuf add silence %d -> %d/%d/%d",
			    n,
			    track->freq.srcbuf.head,
			    track->freq.srcbuf.used,
			    track->freq.srcbuf.capacity);
		}
		if (track->freq.srcbuf.used > 0) {
			audio_apply_stage(track, &track->freq, true);
		}
	}

	if (dropcount != 0) {
		// 変換が1ブロックぴったりでない場合、変換バッファのポインタが
		// バッファ途中を指すことになるのでこれをクリアする。
		// ここの変換バッファは、前後のリングバッファとの対称性で
		// リングバッファの形にしてあるが運用上はただのバッファなので、
		// ポインタが途中を指されていると困る。
		// これが起きるのは PLAY_SYNC か drain 中の時。
		/*
		 * Clear all conversion buffer pointer if the conversion was
		 * not exactly one block.  These conversion stage buffers are
		 * certainly circular buffers because of symmetry with the
		 * previous and next stage buffer.  However, since they are
		 * treated as simple contiguous buffers in operation, so head
		 * always should point 0.  This may happen during PLAY(_SYNC)
		 * or drain-age.
		 */
		TRACET(track, "reset stage");
		if (track->codec.filter) {
			KASSERT(track->codec.srcbuf.used == 0);
			track->codec.srcbuf.head = 0;
		}
		if (track->chvol.filter) {
			KASSERT(track->chvol.srcbuf.used == 0);
			track->chvol.srcbuf.head = 0;
		}
		if (track->chmix.filter) {
			KASSERT(track->chmix.srcbuf.used == 0);
			track->chmix.srcbuf.head = 0;
		}
		if (track->freq.filter) {
			KASSERT(track->freq.srcbuf.used == 0);
			track->freq.srcbuf.head = 0;
		}
	}

	if (track->input == &track->outbuf) {
		track->outputcounter = track->inputcounter;
	} else {
		track->outputcounter += track->outbuf.used - track_count_0;
	}

#if AUDIO_DEBUG > 2
	struct audio_track_debugbuf m;
	audio_track_bufstat(track, &m);
	TRACET(track, "end%s%s%s%s%s%s",
	    m.outbuf, m.freq, m.chvol, m.chmix, m.codec, m.usrbuf);
#endif
}

// 録音時、ミキサーによってトラックの input に渡されたブロックを
// usrbuf まで変換します。
static void
audio_track_record(audio_track_t *track)
{
	audio_ring_t *outbuf;
	audio_ring_t *usrbuf;
	int count;
	int bytes;
	int framesize;

	KASSERT(track);

	// 処理するフレーム数
	/* number of frames to process */
	count = auring_get_contig_used(track->input);
	count = min(count, track->mixer->frames_per_block);
	if (count == 0) {
		TRACET(track, "count == 0");
		return;
	}

	// 周波数変換
	/* Frequency conversion */
	if (track->freq.filter) {
		if (track->freq.srcbuf.used > 0) {
			audio_apply_stage(track, &track->freq, true);
			// XXX freq の入力は先頭からでなくてよいか?
		}
	}

	// チャンネルミキサ
	/* Channel Mix */
	if (track->chmix.filter)
		audio_apply_stage(track, &track->chmix, false);

	// チャンネルボリューム
	/* Channel Volume */
	if (track->chvol.filter)
		audio_apply_stage(track, &track->chvol, false);

	// エンコーディング変換
	/* Encoding Conversion */
	if (track->codec.filter)
		audio_apply_stage(track, &track->codec, false);

	// outbuf から usrbuf へ
	/* Copy from outbuf to usrbuf */
	outbuf = &track->outbuf;
	usrbuf = &track->usrbuf;
	// 出力(outbuf)に 4bit は来ないので1フレームは必ず1バイト以上ある
	/*
	 * framesize is always 1 byte or more since all formats supported as
	 * usrfmt(=output) have 8bit or more stride.
	 */
	framesize = frametobyte(&outbuf->fmt, 1);
	KASSERT(framesize >= 1);
	// count は usrbuf にコピーするフレーム数。
	// bytes は usrbuf にコピーするバイト数。
	/*
	 * count is the number of frames to copy to usrbuf.
	 * bytes is the number of bytes to copy to usrbuf.
	 */
	count = outbuf->used;
	count = min(count,
	    (track->usrbuf_usedhigh - usrbuf->used) / framesize);
	bytes = count * framesize;
	if (auring_tail(usrbuf) + bytes < usrbuf->capacity) {
		memcpy((uint8_t *)usrbuf->mem + auring_tail(usrbuf),
		    (uint8_t *)outbuf->mem + outbuf->head * framesize,
		    bytes);
		auring_push(usrbuf, bytes);
		auring_take(outbuf, count);
	} else {
		int bytes1;
		int bytes2;

		bytes1 = auring_get_contig_used(usrbuf);
		KASSERT(bytes1 % framesize == 0);
		memcpy((uint8_t *)usrbuf->mem + auring_tail(usrbuf),
		    (uint8_t *)outbuf->mem + outbuf->head * framesize,
		    bytes1);
		auring_push(usrbuf, bytes1);
		auring_take(outbuf, bytes1 / framesize);

		bytes2 = bytes - bytes1;
		memcpy((uint8_t *)usrbuf->mem + auring_tail(usrbuf),
		    (uint8_t *)outbuf->mem + outbuf->head * framesize,
		    bytes2);
		auring_push(usrbuf, bytes2);
		auring_take(outbuf, bytes2 / framesize);
	}

	// XXX カウンタ

#if AUDIO_DEBUG > 2
	struct audio_track_debugbuf m;
	audio_track_bufstat(track, &m);
	TRACET(track, "end%s%s%s%s%s%s",
	    m.freq, m.chvol, m.chmix, m.codec, m.outbuf, m.usrbuf);
#endif
}

// blktime は1ブロックの時間 [msec]。
//
// 例えば HW freq = 44100 に対して、
// blktime 50 msec は 2205 frame/block でこれは割りきれるので問題ないが、
// blktime 25 msec は 1102.5 frame/block となり、フレーム数が整数にならない。
// この場合 frame/block を切り捨てるなり切り上げるなりすれば整数にはなる。
// 例えば切り捨てて 1102 frame/block とするとこれに相当する1ブロックの時間は
// 24.9886… [msec] と割りきれなくなる。周波数がシステム中で1つしかなければ
// これでも構わないが、AUDIO2 ではブロック単位で周波数変換を行うため極力
// 整数にしておきたい (整数にしておいても割りきれないケースは出るが後述)。
//
// ここではより多くの周波数に対して frame/block が整数になりやすいよう
// AUDIO_BLK_MS の初期値を 40 msec に設定してある。
//   8000 [Hz] * 40 [msec] = 320 [frame/block] (8000Hz - 48000Hz 系)
//  11025 [Hz] * 40 [msec] = 441 [frame/block] (44100Hz 系)
//  15625 [Hz] * 40 [msec] = 625 [frame/block]
//
// これにより主要な周波数についてはわりと誤差(端数)なく周波数変換が行える。
// 例えば 44100 [Hz] を 48000 [Hz] に変換する場合 40 [msec] ブロックなら
//  44100 [Hz] * 40 [msec] = 1764 [frame/block]
//                           1920 [frame/block] = 48000 [Hz] * 40 [msec]
// となり、1764 フレームを 1920 フレームに変換すればよいことになる。
// ただし、入力周波数も HW 周波数も任意であるため、周波数変換前後で
// frame/block が必ずしもきりのよい値になるとは限らないが、そこはどのみち
// 仕方ない。(あくまで、主要な周波数で割り切れやすい、ということ)
//
// また、いくつかの変態ハードウェアではさらに手当てが必要。
//
// 1) vs(4) x68k MSM6258 ADPCM
//  vs(4) は 15625 Hz、4bit、1channel である。このため
//  blktime 40 [msec] は 625 [frame/block] と割りきれる値になるが、これは
//  同時に 312.5 [byte/block] であり、バイト数が割りきれないため、これは不可。
//  blktime 80 [msec] であれば 1250 [frame/block] = 625 [byte/block] となる
//  のでこれなら可。
//  vs(4) 以外はすべて stride が 8 の倍数なので、この「frame/block は割り
//  切れるのに byte/block にすると割りきれない」問題は起きない。
//
//  # 世の中には 3bit per frame とかいう ADPCM もあるにはあるが、
//  # 現行 NetBSD はこれをサポートしておらず、今更今後サポートするとも
//  # 思えないのでこれについては考慮しない。やりたい人がいたら頑張って。
//
// 2) aucc(4) amiga
//  周波数が変態だが詳細未調査。

// mixer(.hwbuf.fmt) から blktime [msec] を計算します。
/*
 * Calcurate blktime [msec] from mixer(.hwbuf.fmt).
 */
static u_int
audio_mixer_calc_blktime(audio_trackmixer_t *mixer)
{
	audio_format2_t *fmt;
	u_int blktime;
	u_int frames_per_block;

	fmt = &mixer->hwbuf.fmt;

	// XXX とりあえず手抜き実装。あとでかんがえる

	blktime = AUDIO_BLK_MS;

	// 8 の倍数以外の stride は今のところ 4 しかない。
	if (fmt->stride == 4) {
		frames_per_block = fmt->sample_rate * blktime / 1000;
		if ((frames_per_block & 1) != 0)
			blktime *= 2;
	}
#ifdef DIAGNOSTIC
	else if (fmt->stride % NBBY != 0) {
		panic("unsupported HW stride %d", fmt->stride);
	}
#endif

	return blktime;
}

// ミキサを初期化します。
// mixer はゼロフィルされているものとします。
// mode は再生なら AUMODE_PLAY、録音なら AUMODE_RECORD を指定します。
// 単に録音再生のどちら側かだけなので AUMODE_PLAY_ALL は関係ありません。
// sc_lock でコールします。
/*
 * Initialize the mixer.
 * This function returns 0 on sucessful.  Otherwise returns errno.
 * 'mixer' should be zero-filled.
 * For 'mode', specify AUMODE_PLAY for playback, AUMODE_RECORD for record
 * (AUMODE_PLAY_ALL does not matter here).
 * This function should be called with sc_lock.
 */
static int
audio_mixer_init(struct audio_softc *sc, int mode, const audio_format2_t *hwfmt)
{
	audio_trackmixer_t *mixer;
	int len;
	int blksize;
	int capacity;
	size_t bufsize;
	int error;

printf("%s\n", __func__);
	error = 0;
	if (mode == AUMODE_PLAY)
		mixer = sc->sc_pmixer;
	else
		mixer = sc->sc_rmixer;

	mixer->sc = sc;
	mixer->mode = mode;

	/* draincv is used only for playback */
	if (mode == AUMODE_PLAY)
		cv_init(&mixer->draincv, "audiodr");

	mixer->hwbuf.fmt = *hwfmt;
	mixer->volume = 256;
	mixer->blktime_d = 1000;
	mixer->blktime_n = audio_mixer_calc_blktime(mixer);
	mixer->hwblks = NBLKHW;

	mixer->frames_per_block = frame_per_block(mixer, &mixer->hwbuf.fmt);
	blksize = frametobyte(&mixer->hwbuf.fmt, mixer->frames_per_block);
	if (sc->hw_if->round_blocksize) {
		int rounded;
		audio_params_t p = format2_to_params(&mixer->hwbuf.fmt);
		rounded = sc->hw_if->round_blocksize(sc->hw_hdl, blksize,
		    mode, &p);
		// 違っていても困る?
		if (rounded != blksize) {
			if ((rounded * NBBY) % (mixer->hwbuf.fmt.stride *
			    mixer->hwbuf.fmt.channels) != 0) {
				aprint_error_dev(sc->dev,
				    "blksize not configured %d -> %d\n",
				    blksize, rounded);
				return ENXIO;
			}
			// 再計算
			mixer->frames_per_block = rounded * NBBY /
			    (mixer->hwbuf.fmt.stride *
			     mixer->hwbuf.fmt.channels);
		}
	}
	mixer->blktime_n = mixer->frames_per_block;
	mixer->blktime_d = mixer->hwbuf.fmt.sample_rate;

	capacity = mixer->frames_per_block * mixer->hwblks;
	bufsize = frametobyte(&mixer->hwbuf.fmt, capacity);
	if (sc->hw_if->round_buffersize) {
		size_t rounded;
		rounded = sc->hw_if->round_buffersize(sc->hw_hdl, mode,
		    bufsize);
		// 縮められても困る?
		if (rounded != bufsize) {
			aprint_error_dev(sc->dev,
			    "buffer size not configured %zu -> %zu\n",
			    bufsize, rounded);
			return ENXIO;
		}
	}
	mixer->hwbuf.capacity = capacity;

	if (sc->hw_if->allocm) {
		mixer->hwbuf.mem = sc->hw_if->allocm(sc->hw_hdl, mode, bufsize);
		if (mixer->hwbuf.mem == NULL) {
			aprint_error_dev(sc->dev, "%s: allocm(%zu) failed\n",
			    __func__, bufsize);
			return ENOMEM;
		}
	} else {
		mixer->hwbuf.mem = kern_malloc(bufsize, M_NOWAIT);
		if (mixer->hwbuf.mem == NULL) {
			aprint_error_dev(sc->dev,
			    "%s: malloc hwbuf(%zu) failed\n",
			    __func__, bufsize);
			return ENOMEM;
		}
	}

	mixer->track_fmt.encoding = AUDIO_ENCODING_SLINEAR_NE;
	mixer->track_fmt.precision = AUDIO_INTERNAL_BITS;
	mixer->track_fmt.stride = AUDIO_INTERNAL_BITS;
	mixer->track_fmt.channels = mixer->hwbuf.fmt.channels;
	mixer->track_fmt.sample_rate = mixer->hwbuf.fmt.sample_rate;

	if (mode == AUMODE_PLAY) {
		// 合成バッファ
		mixer->mixfmt = mixer->track_fmt;
		mixer->mixfmt.precision *= 2;
		mixer->mixfmt.stride *= 2;
		// XXX マクロとか使えないんだっけ
		len = mixer->frames_per_block * mixer->mixfmt.channels *
		    mixer->mixfmt.stride / NBBY;
		mixer->mixsample = audio_realloc(mixer->mixsample, len);
		if (mixer->mixsample == NULL) {
			aprint_error_dev(sc->dev,
			    "%s: malloc mixsample(%d) failed\n",
			    __func__, len);
			error = ENOMEM;
			goto abort;
		}
	} else {
		// 合成バッファは使用しない
	}

	// XXX どうするか
	audio_filter_reg_t *reg;
	if (mode == AUMODE_PLAY) {
		reg = &sc->sc_xxx_pfilreg;
	} else {
		reg = &sc->sc_xxx_rfilreg;
	}
	mixer->codec = reg->codec;
	if (mixer->codec) {
		mixer->codecarg.context = reg->context;
		if (mode == AUMODE_PLAY) {
			mixer->codecarg.srcfmt = &mixer->track_fmt;
			mixer->codecarg.dstfmt = &mixer->hwbuf.fmt;
		} else {
			mixer->codecarg.srcfmt = &mixer->hwbuf.fmt;
			mixer->codecarg.dstfmt = &mixer->track_fmt;
		}
		mixer->codecbuf.fmt = mixer->track_fmt;
		mixer->codecbuf.capacity = mixer->frames_per_block;
		len = auring_bytelen(&mixer->codecbuf);
		mixer->codecbuf.mem = audio_realloc(mixer->codecbuf.mem, len);
		if (mixer->codecbuf.mem == NULL) {
			aprint_error_dev(sc->dev,
			    "%s: malloc codecbuf(%d) failed\n",
			    __func__, len);
			error = ENOMEM;
			goto abort;
		}
	}

	return 0;

abort:
	audio_mixer_destroy(sc, mixer);
	return error;
}

// ミキサを終了しリソースを解放します。
// mixer 自身のメモリは解放しません。
/*
 * Release all resources of 'mixer'.
 * Note that it does not release the memory area of 'mixer' itself.
 */
static void
audio_mixer_destroy(struct audio_softc *sc, audio_trackmixer_t *mixer)
{
	int mode;

	mode = mixer->mode;
	KASSERT(mode == AUMODE_PLAY || mode == AUMODE_RECORD);

	if (mixer->hwbuf.mem != NULL) {
		if (sc->hw_if->freem) {
			sc->hw_if->freem(sc->hw_hdl, mixer->hwbuf.mem, mode);
		} else {
			kern_free(mixer->hwbuf.mem);
		}
		mixer->hwbuf.mem = NULL;
	}

	audio_free(mixer->codecbuf.mem);
	audio_free(mixer->mixsample);

	if (mode == AUMODE_PLAY)
		cv_destroy(&mixer->draincv);
}

// 再生ミキサを起動します。
// 割り込みコンテキストから呼び出してはいけません。
/*
 * Starts playback mixer.
 * This function does nothing if the mixer has already started.
 * This function must not be called from the interrupt context.
 */
static void
audio_pmixer_start(struct audio_softc *sc, bool force)
{
	audio_trackmixer_t *mixer;
	int minimum;

	KASSERT(mutex_owned(sc->sc_lock));

	/* Return if the mixer has already started. */
	if (sc->sc_pbusy)
		return;

	mixer = sc->sc_pmixer;
	TRACE("begin mixseq=%d hwseq=%d hwbuf=%d/%d/%d%s",
	    (int)mixer->mixseq, (int)mixer->hwseq,
	    mixer->hwbuf.head, mixer->hwbuf.used, mixer->hwbuf.capacity,
	    force ? " force" : "");

	// 通常は2ブロック貯めてから再生開始したい。
	// force ならそういうわけにいかないので1ブロックで再生開始する。
	minimum = (force) ? 1 : 2;
	while (mixer->hwbuf.used < mixer->frames_per_block * minimum) {
		audio_pmixer_process(sc, false);
	}

	// トラックミキサ出力開始
	/* Start output */
	mutex_enter(sc->sc_intr_lock);
	audio_pmixer_output(sc);
	mutex_exit(sc->sc_intr_lock);

	TRACE("end   mixseq=%d hwseq=%d hwbuf=%d/%d/%d",
	    (int)mixer->mixseq, (int)mixer->hwseq,
	    mixer->hwbuf.head, mixer->hwbuf.used, mixer->hwbuf.capacity);
}

/*
 * When playing back with MD filter:
 *
 *           track track ...
 *               v v
 *                +  mix (with aint2_t)
 *                |  master volume (with aint2_t)
 *                v
 *    mixsample [::::]                  wide-int 1 block (ring) buffer
 *                |
 *                |  convert aint2_t -> aint_t
 *                v
 *    codecbuf  [....]                  1 block (ring) buffer
 *                |
 *                |  convert to hw format
 *                v
 *    hwbuf     [............]          NBLKHW blocks ring buffer
 *
 * When playingback without MD filter:
 *
 *    mixsample [::::]                  wide-int 1 block (ring) buffer
 *                |
 *                |  convert aint2_t -> aint_t
 *                v
 *    hwbuf     [............]          NBLKHW blocks ring buffer
 *
 * mixsample: slinear_NE, wide internal precision, HW ch, HW freq.
 * codecbuf:  slinear_NE, internal precision,      HW ch, HW freq.
 * hwbuf:     HW encoding, HW precision,           HW ch, HW freq.
 */

// 全トラックを倍精度ミキシングバッファで合成し、
// 倍精度ミキシングバッファから hwbuf への変換を行います。
// (hwbuf からハードウェアへの転送はここでは行いません)
// 呼び出し時の sc_intr_lock の状態はどちらでもよく、hwbuf へのアクセスを
// sc_intr_lock でこの関数が保護します。
// intr が true なら割り込みコンテキストからの呼び出し、
// false ならプロセスコンテキストからの呼び出しを示す。
/*
 * Perform mixing and convert it to hwbuf.
 * Note that this function doesn't transfer from hwbuf to HW.
 * If 'isintr' is true, it indicates a call from the interrupt context.
 * If false, it indicates a call from process context.
 * It can be called with or without sc_intr_lock.
 */
static void
audio_pmixer_process(struct audio_softc *sc, bool isintr)
{
	audio_trackmixer_t *mixer;
	int hw_free_count;
	int frame_count;
	int sample_count;
	int mixed;
	int lock_owned;
	int i;
	aint2_t *m;
	aint_t *h;

	mixer = sc->sc_pmixer;

	// 今回取り出すフレーム数を決定
	// 実際には hwbuf はブロック単位で変動するはずなので
	// count は1ブロック分になるはず
	hw_free_count = auring_get_contig_free(&mixer->hwbuf);
	frame_count = min(hw_free_count, mixer->frames_per_block);
	if (frame_count <= 0) {
		TRACE("count too short: hw_free=%d frames_per_block=%d",
		    hw_free_count, mixer->frames_per_block);
		return;
	}
	sample_count = frame_count * mixer->mixfmt.channels;

	mixer->mixseq++;

	// 全トラックを合成
	mixed = audio_pmixer_mixall(sc, isintr);
	if (mixed == 0) {
		// 無音
		memset(mixer->mixsample, 0,
		    frametobyte(&mixer->mixfmt, frame_count));
	} else {
		aint2_t ovf_plus;
		aint2_t ovf_minus;
		int vol;

		// オーバーフロー検出
		/* Overflow detection */
		ovf_plus = AINT_T_MAX;
		ovf_minus = AINT_T_MIN;
		m = mixer->mixsample;
		for (i = 0; i < sample_count; i++) {
			aint2_t val;

			val = *m++;
			if (val > ovf_plus)
				ovf_plus = val;
			else if (val < ovf_minus)
				ovf_minus = val;
		}

		// マスタボリュームの自動制御
		/* Master Volume Auto Adjust */
		vol = mixer->volume;
		if (ovf_plus > (aint2_t)AINT_T_MAX
		 || ovf_minus < (aint2_t)AINT_T_MIN) {
			aint2_t ovf;
			int vol2;

			// TODO: AINT2_T_MIN チェック?
			ovf = ovf_plus;
			if (ovf < -ovf_minus)
				ovf = -ovf_minus;

			// オーバーフローしてたら少なくとも今回はボリュームを
			// 下げる
			/* Turn down the volume if overflow occured. */
			vol2 = (int)((aint2_t)AINT_T_MAX * 256 / ovf);
			if (vol2 < vol)
				vol = vol2;

			if (vol < mixer->volume) {
				// 128 までは自動でマスタボリュームを下げる
				// 今の値の 95% ずつに下げていってみる
				/* Turn down gradually to 128. */
				if (mixer->volume > 128) {
					mixer->volume = mixer->volume * 95 / 100;
					aprint_normal_dev(sc->dev,
					    "auto volume adjust: volume %d\n",
					    mixer->volume);
				}
			}
		}

		// マスタボリューム適用
		/* Apply Master Volume. */
		if (vol != 256) {
			m = mixer->mixsample;
			for (i = 0; i < sample_count; i++) {
#if defined(AUDIO_USE_C_IMPLEMENTATION_DEFINED_BEHAVIOR) && defined(__GNUC__)
				*m = *m * vol >> 8;
#else
				*m = *m * vol / 256;
#endif
				m++;
			}
		}
	}

	// ここから ハードウェアチャンネル

	// ハードウェアバッファへ転送
	lock_owned = mutex_tryenter(sc->sc_intr_lock);

	m = mixer->mixsample;
	// MD 側フィルタがあれば aint2_t -> aint_t を codecbuf へ
	if (mixer->codec) {
		h = auring_tailptr_aint(&mixer->codecbuf);
	} else {
		h = auring_tailptr_aint(&mixer->hwbuf);
	}

	for (i = 0; i < sample_count; i++) {
		*h++ = *m++;
	}

	// MD 側フィルタ
	if (mixer->codec) {
		auring_push(&mixer->codecbuf, frame_count);
		mixer->codecarg.src = auring_headptr(&mixer->codecbuf);
		mixer->codecarg.dst = auring_tailptr(&mixer->hwbuf);
		mixer->codecarg.count = frame_count;
		mixer->codec(&mixer->codecarg);
		auring_take(&mixer->codecbuf, mixer->codecarg.count);
	}

	auring_push(&mixer->hwbuf, frame_count);

	TRACE("done mixseq=%d hwbuf=%d/%d/%d%s",
	    (int)mixer->mixseq,
	    mixer->hwbuf.head, mixer->hwbuf.used, mixer->hwbuf.capacity,
	    (mixed == 0) ? " silent" : "");

	if (lock_owned) {
		mutex_exit(sc->sc_intr_lock);
	}
}

// 全トラックを 1ブロック分合成します。
// 合成されたトラック数を返します。
/*
 * Mix all tracks one block.
 * It returns the number of mixed tracks.
 */
static int
audio_pmixer_mixall(struct audio_softc *sc, bool isintr)
{
	audio_trackmixer_t *mixer;
	audio_file_t *f;
	int req;
	int mixed;

	mixer = sc->sc_pmixer;

	// XXX frames_per_block そのままのほうが分かりやすいような
	req = mixer->frames_per_block;

	mixed = 0;
	SLIST_FOREACH(f, &sc->sc_files, entry) {
		audio_track_t *track = f->ptrack;

		if (track == NULL)
			continue;

		// 協調的ロックされているトラックは、今回ミキシングしない。
		if (isintr && track->in_use) {
			TRACET(track, "skip; in use");
			continue;
		}

		if (track->is_pause) {
			TRACET(track, "skip; paused");
			continue;
		}

		// mmap トラックならここで入力があったことにみせかける
		if (track->mmapped) {
			// XXX push じゃなく直接操作してウィンドウを移動みたいに
			// したほうがいいんじゃないか。
			auring_push(&track->usrbuf, track->usrbuf_blksize);
			TRACET(track, "mmap; usr=%d/%d/C%d",
			    track->usrbuf.head,
			    track->usrbuf.used,
			    track->usrbuf.capacity);
		}

		if (track->outbuf.used < req && track->usrbuf.used > 0) {
			TRACET(track, "process");
			audio_track_play(track);
		}

		if (track->outbuf.used == 0) {
			TRACET(track, "skip; empty");
			continue;
		}
		// 合成
		mixed = audio_pmixer_mix_track(mixer, track, req, mixed);
	}
	return mixed;
}

// トラックバッファから取り出し、ミキシングします。
// mixed には呼び出し時点までの合成済みトラック数を渡します。
// 戻り値はこの関数終了時での合成済みトラック数)です。
// つまりこのトラックを合成すれば mixed + 1 を返します。
/*
 * Mix one track.
 * 'mixed' specifies the number of tracks mixed so far.
 * It returns the number of tracks mixed.  In other words, it returns
 * mixed+1 if this track is mixed.
 */
static int
audio_pmixer_mix_track(audio_trackmixer_t *mixer, audio_track_t *track,
	int req, int mixed)
{
	int count;
	int sample_count;
	int remain;
	int i;
	const aint_t *s;
	aint2_t *d;

	// 現時点で outbuf に溜まってるやつを最大1ブロック分処理する。

	// このトラックが処理済みならなにもしない
	if (mixer->mixseq < track->seq) return mixed;

	count = auring_get_contig_used(&track->outbuf);
	count = min(count, mixer->frames_per_block);

	s = auring_headptr_aint(&track->outbuf);
	d = mixer->mixsample;

	// 整数倍精度へ変換し、トラックボリュームを適用して加算合成
	sample_count = count * mixer->mixfmt.channels;
	if (mixed == 0) {
		// 最初のトラック合成は代入
		if (track->volume == 256) {
			for (i = 0; i < sample_count; i++) {
				*d++ = ((aint2_t)*s++);
			}
		} else {
			for (i = 0; i < sample_count; i++) {
#if defined(AUDIO_USE_C_IMPLEMENTATION_DEFINED_BEHAVIOR) && defined(__GNUC__)
				*d++ = ((aint2_t)*s++) * track->volume >> 8;
#else
				*d++ = ((aint2_t)*s++) * track->volume / 256;
#endif
			}
		}
	} else {
		// 2本め以降なら加算合成
		if (track->volume == 256) {
			for (i = 0; i < sample_count; i++) {
				*d++ += ((aint2_t)*s++);
			}
		} else {
			for (i = 0; i < sample_count; i++) {
#if defined(AUDIO_USE_C_IMPLEMENTATION_DEFINED_BEHAVIOR) && defined(__GNUC__)
				*d++ += ((aint2_t)*s++) * track->volume >> 8;
#else
				*d++ += ((aint2_t)*s++) * track->volume / 256;
#endif
			}
		}
	}

	auring_take(&track->outbuf, count);
	// outbuf が1ブロック未満であっても、カウンタはブロック境界に
	// いなければならないため、不足が出れば調整する。
	remain = mixer->frames_per_block - count;
	if (__predict_false(remain != 0)) {
		auring_push(&track->outbuf, remain);
		auring_take(&track->outbuf, remain);
	}

	// トラックバッファを取り込んだことを反映
	// mixseq はこの時点ではまだ前回の値なのでトラック側へは +1
	track->seq = mixer->mixseq + 1;

	// audio_write() に空きが出来たことを通知
	cv_broadcast(&track->outchan);

	TRACET(track, "broadcast; trseq=%d out=%d/%d/%d", (int)track->seq,
	    track->outbuf.head, track->outbuf.used, track->outbuf.capacity);

	// usrbuf が空いたら(lowat を下回ったら) シグナルを送る
	// XXX ここで usrbuf が空いたかどうかを見るのもどうかと思うが
	if (track->usrbuf.used <= track->usrbuf_usedlow && !track->is_pause) {
		if (track->sih_wr) {
			kpreempt_disable();
			softint_schedule(track->sih_wr);
			kpreempt_enable();
		}
	}

	return mixed + 1;
}

// ハードウェアバッファから 1 ブロック出力します。
// sc_intr_lock で呼び出してください。
/*
 * Output one block from hwbuf to HW.
 * It must be called with sc_intr_lock.
 */
static void
audio_pmixer_output(struct audio_softc *sc)
{
	audio_trackmixer_t *mixer;
	audio_params_t params;
	void *start;
	void *end;
	int blksize;
	int error;

	mixer = sc->sc_pmixer;
	TRACE("pbusy=%d hwbuf=%d/%d/%d",
	    sc->sc_pbusy,
	    mixer->hwbuf.head, mixer->hwbuf.used, mixer->hwbuf.capacity);
	KASSERT(mixer->hwbuf.used >= mixer->frames_per_block);

	blksize = frametobyte(&mixer->hwbuf.fmt, mixer->frames_per_block);

	if (sc->hw_if->trigger_output) {
		/* trigger (at once) */
		if (!sc->sc_pbusy) {
			start = mixer->hwbuf.mem;
			end = (uint8_t *)start + auring_bytelen(&mixer->hwbuf);
			params = format2_to_params(&mixer->hwbuf.fmt);

			error = sc->hw_if->trigger_output(sc->hw_hdl,
			    start, end, blksize, audio_pintr, sc, &params);
			if (error) {
				DPRINTF(1, "%s trigger_output failed: %d\n",
				    __func__, error);
				return;
			}
		}
	} else {
		/* start (everytime) */
		start = auring_headptr(&mixer->hwbuf);

		error = sc->hw_if->start_output(sc->hw_hdl,
		    start, blksize, audio_pintr, sc);
		if (error) {
			DPRINTF(1, "%s start_output failed: %d\n",
			    __func__, error);
			return;
		}
	}
#if defined(_KERNEL)
	// ユーザランドテストでは pbusy を立てずに毎回 start を起こさせる
	sc->sc_pbusy = true;
#endif
}

// 割り込みハンドラです。
// sc_intr_lock で呼び出されます。
/*
 * This is an interrupt handler for playback.
 * It is called with sc_intr_lock.
 */
static void
audio_pintr(void *arg)
{
	struct audio_softc *sc;
	audio_trackmixer_t *mixer;

	sc = arg;
	KASSERT(mutex_owned(sc->sc_intr_lock));

	mixer = sc->sc_pmixer;
	mixer->hw_complete_counter += mixer->frames_per_block;
	mixer->hwseq++;

	auring_take(&mixer->hwbuf, mixer->frames_per_block);

	TRACE("HW_INT ++hwseq=%d cmplcnt=%d hwbuf=%d/%d/%d",
	    (int)mixer->hwseq, (int)mixer->hw_complete_counter,
	    mixer->hwbuf.head, mixer->hwbuf.used, mixer->hwbuf.capacity);

#if !defined(_KERNEL)
	// ユーザランドエミュレーションは割り込み駆動ではないので
	// 処理はここまで。
	return;
#endif

#if defined(AUDIO_HW_SINGLE_BUFFER)
	// その場で次のブロックを作成して再生
	// レイテンシは下げられるが、マシンパワーがないと再生が途切れる。

	// バッファを作成
	audio_pmixer_process(sc, true);

	// 出力
	audio_pmixer_output(sc);
#else
	// ブロック0 の出力が終わったら用意されているはずのブロック1 を
	// すぐに出力しておいて、その後ブロック2 の作成に取りかかる。
	// これで遅マシンでも再生を途切れさせずに HW ブロックが作成できるが、
	// その代わりレイテンシは1ブロック分上がる。

	// まず出力待ちのシーケンスを出力
	if (mixer->hwbuf.used >= mixer->frames_per_block) {
		audio_pmixer_output(sc);
	}

	bool later = false;

	if (mixer->hwbuf.used < mixer->frames_per_block) {
		later = true;
	}

	// 次のバッファを用意する
	audio_pmixer_process(sc, true);

	if (later) {
		audio_pmixer_output(sc);
	}
#endif

	// drain 待ちしている人のために通知
	cv_broadcast(&mixer->draincv);
}

// 録音ミキサを起動します。
// 割り込みコンテキストから呼び出してはいけません。
/*
 * Starts record mixer.
 * This function does nothing if the mixer has already started.
 * This function must not be called from the interrupt context.
 */
static void
audio_rmixer_start(struct audio_softc *sc)
{

	KASSERT(mutex_owned(sc->sc_lock));

	// すでに再生ミキサが起動していたら、true を返す
	if (sc->sc_rbusy)
		return;

	TRACE("begin");

	mutex_enter(sc->sc_intr_lock);
	audio_rmixer_input(sc);
	mutex_exit(sc->sc_intr_lock);
}

/*
 * When recording with MD filter:
 *
 *    hwbuf     [............]          NBLKHW blocks ring buffer
 *                |
 *                | convert from hw format
 *                v
 *    codecbuf  [....]                  1 block (ring) buffer
 *               |  |
 *               v  v
 *            track track ...
 *
 * When recording without MD filter:
 *
 *    hwbuf     [............]          NBLKHW blocks ring buffer
 *               |  |
 *               v  v
 *            track track ...
 *
 * hwbuf:     HW encoding, HW precision, HW ch, HW freq.
 * codecbuf:  slinear_NE, internal precision, HW ch, HW freq.
 */

// 録音できた hwbuf のブロックを全録音トラックへ分配します。
/*
 * Distribute a recorded block to all recoding tracks.
 */
static void
audio_rmixer_process(struct audio_softc *sc)
{
	audio_trackmixer_t *mixer;
	audio_ring_t *mixersrc;
	audio_file_t *f;
	int count;
	int bytes;

	mixer = sc->sc_rmixer;

	// 今回取り出すフレーム数を決定
	// 実際には hwbuf はブロック単位で変動するはずなので
	// count は1ブロック分になるはず
	count = auring_get_contig_used(&mixer->hwbuf);
	count = min(count, mixer->frames_per_block);
	if (count <= 0) {
		TRACE("count %d: too short", count);
		return;
	}
	bytes = frametobyte(&mixer->track_fmt, count);

	// MD 側フィルタ
	if (mixer->codec) {
		mixer->codecarg.src = auring_headptr(&mixer->hwbuf);
		mixer->codecarg.dst = auring_tailptr(&mixer->codecbuf);
		mixer->codecarg.count = count;
		mixer->codec(&mixer->codecarg);
		auring_take(&mixer->hwbuf, mixer->codecarg.count);
		auring_push(&mixer->codecbuf, mixer->codecarg.count);
		mixersrc = &mixer->codecbuf;
	} else {
		mixersrc = &mixer->hwbuf;
	}

	// 全トラックへ分配
	SLIST_FOREACH(f, &sc->sc_files, entry) {
		audio_track_t *track = f->rtrack;
		audio_ring_t *input;

		if (track == NULL)
			continue;

		if (track->in_use) {
			TRACET(track, "skip; in use");
			continue;
		}

		if (track->is_pause) {
			TRACET(track, "skip; paused");
			continue;
		}

		// 空いてなければ古い方から捨てる?
		input = track->input;
		if (input->capacity - input->used < mixer->frames_per_block) {
			int drops = mixer->frames_per_block -
			    (input->capacity - input->used);
			track->dropframes += drops;
			TRACET(track, "drop %d frames: inp=%d/%d/%d",
			    drops,
			    input->head, input->used, input->capacity);
			auring_take(input, drops);
		}
		KASSERT(input->used % mixer->frames_per_block == 0);

		memcpy(auring_tailptr_aint(input),
		    auring_headptr_aint(mixersrc),
		    bytes);
		auring_push(input, count);

		// XXX シーケンスいるんだっけ

		// audio_read() にブロックが来たことを通知
		cv_broadcast(&track->outchan);

		TRACET(track, "broadcast; inp=%d/%d/%d",
		    input->head, input->used, input->capacity);
	}

	auring_take(mixersrc, count);

	// SIGIO を通知(する必要があるかどうかは向こうで判断する)
	// XXX トラック別にしなくていいか
	softint_schedule(sc->sc_sih_rd);
}

// ハードウェアバッファに1ブロック入力を開始します。
// sc_intr_lock で呼び出してください。
/*
 * Input one block from HW to hwbuf.
 * It must be called with sc_intr_lock.
 */
static void
audio_rmixer_input(struct audio_softc *sc)
{
	audio_trackmixer_t *mixer;
	audio_params_t params;
	void *start;
	void *end;
	int blksize;
	int error;

	mixer = sc->sc_rmixer;
	blksize = frametobyte(&mixer->hwbuf.fmt, mixer->frames_per_block);

	if (sc->hw_if->trigger_input) {
		/* trigger (at once) */
		if (!sc->sc_rbusy) {
			start = mixer->hwbuf.mem;
			end = (uint8_t *)start + auring_bytelen(&mixer->hwbuf);
			params = format2_to_params(&mixer->hwbuf.fmt);

			error = sc->hw_if->trigger_input(sc->hw_hdl,
			    start, end, blksize, audio_rintr, sc, &params);
			if (error) {
				DPRINTF(1, "%s trigger_input failed: %d\n",
				    __func__, error);
				return;
			}
		}
	} else {
		/* start (everytime) */
		start = auring_tailptr(&mixer->hwbuf);

		error = sc->hw_if->start_input(sc->hw_hdl,
		    start, blksize, audio_rintr, sc);
		if (error) {
			DPRINTF(1, "%s start_input failed: %d\n",
			    __func__, error);
			return;
		}
	}
	sc->sc_rbusy = true;
}

// 割り込みハンドラです。
// sc_intr_lock で呼び出されます。
/*
 * This is an interrupt handler for recording.
 * It is called with sc_intr_lock.
 */
static void
audio_rintr(void *arg)
{
	struct audio_softc *sc;
	audio_trackmixer_t *mixer;

	sc = arg;
	KASSERT(mutex_owned(sc->sc_intr_lock));

	mixer = sc->sc_rmixer;
	mixer->hw_complete_counter += mixer->frames_per_block;
	mixer->hwseq++;

	auring_push(&mixer->hwbuf, mixer->frames_per_block);

	TRACE("HW_INT ++hwseq=%d cmplcnt=%d hwbuf=%d/%d/%d",
	    (int)mixer->hwseq, (int)mixer->hw_complete_counter,
	    mixer->hwbuf.head, mixer->hwbuf.used, mixer->hwbuf.capacity);

	// このバッファを分配する
	audio_rmixer_process(sc);

	// 次のバッファを要求
	audio_rmixer_input(sc);
}

// 再生ミキサを停止します。
// 関連するパラメータもクリアするため、基本的には halt_output を
// 直接呼び出すのではなく、こちらを呼んでください。
/*
 * Halt playback mixer.
 * This function also clears related parameters, so call this function
 * instead of calling halt_output directly.
 * This function should be called only when the mixer is running.
 * This funciton should be called with sc_intr_lock.
 */
static int
audio_pmixer_halt(struct audio_softc *sc)
{
	int error;

	TRACE("");
	KASSERT(mutex_owned(sc->sc_lock));
	KASSERT(mutex_owned(sc->sc_intr_lock));

	error = sc->hw_if->halt_output(sc->hw_hdl);
	// エラーが起きても停止は停止する
	sc->sc_pbusy = false;
	sc->sc_pmixer->hwbuf.head = 0;
	sc->sc_pmixer->hwbuf.used = 0;
	sc->sc_pmixer->mixseq = 0;
	sc->sc_pmixer->hwseq = 0;

	return error;
}

// 録音ミキサを停止します。
// 関連するパラメータもクリアするため、基本的には halt_input を
// 直接呼び出すのではなく、こちらを呼んでください。
/*
 * Halt recording mixer.
 * This function also clears related parameters, so call this function
 * instead of calling halt_input directly.
 * This function should be called only when the mixer is running.
 * This funciton should be called with sc_intr_lock.
 */
static int
audio_rmixer_halt(struct audio_softc *sc)
{
	int error;

	TRACE("");
	KASSERT(mutex_owned(sc->sc_lock));
	KASSERT(mutex_owned(sc->sc_intr_lock));

	error = sc->hw_if->halt_input(sc->hw_hdl);
	// エラーが起きても停止は停止する
	sc->sc_rbusy = false;
	sc->sc_rmixer->hwbuf.head = 0;
	sc->sc_rmixer->hwbuf.used = 0;
	sc->sc_rmixer->mixseq = 0;
	sc->sc_rmixer->hwseq = 0;

	return error;
}

// トラックをフラッシュします。
// 現在の動作を停止し、すべてのキューとバッファをクリアし、
// エラーカウンタをリセットします。
// これでええんかなあ。
static void
audio_track_clear(struct audio_softc *sc, audio_track_t *track)
{

	KASSERT(track);
	TRACET(track, "clear");

	KASSERT(mutex_owned(sc->sc_lock));

	track->usrbuf.used = 0;
	// 内部情報も全部クリア
	if (track->codec.filter) {
		track->codec.srcbuf.used = 0;
		track->codec.srcbuf.head = 0;
	}
	if (track->chvol.filter) {
		track->chvol.srcbuf.used = 0;
		track->chvol.srcbuf.head = 0;
	}
	if (track->chmix.filter) {
		track->chmix.srcbuf.used = 0;
		track->chmix.srcbuf.head = 0;
	}
	if (track->freq.filter) {
		track->freq.srcbuf.used = 0;
		track->freq.srcbuf.head = 0;
		if (track->freq_step < 65536)
			track->freq_current = 65536;
		else
			track->freq_current = 0;
		memset(track->freq_prev, 0, sizeof(track->freq_prev));
		memset(track->freq_curr, 0, sizeof(track->freq_curr));
	}
	// バッファをクリアすれば動作は自然と停止する
	mutex_enter(sc->sc_intr_lock);
	track->outbuf.used = 0;
	mutex_exit(sc->sc_intr_lock);

	// カウンタクリア
	track->dropframes = 0;
	track->playdrop = 0;
}

// errno を返します。
/*
 * Drain the track.
 * If successful, it returns 0.  Otherwise returns errno.
 */
static int
audio_track_drain(struct audio_softc *sc, audio_track_t *track)
{
	audio_trackmixer_t *mixer;
	int error;

	KASSERT(track);
	TRACET(track, "start");
	mixer = track->mixer;
	KASSERT(mutex_owned(sc->sc_lock));

	// pause 中なら今溜まってるものは全部無視してこのまま終わってよし
	if (track->is_pause) {
		TRACET(track, "pause -> clear");
		track->pstate = AUDIO_STATE_CLEAR;
	}
	// トラックにデータがなくても drain は ミキサのループが数回回って
	// 問題なく終わるが、クリーンならさすがに早期終了しても
	// いいんじゃなかろうか。
	if (track->pstate == AUDIO_STATE_CLEAR) {
		TRACET(track, "no need to drain");
		return 0;
	}
	track->pstate = AUDIO_STATE_DRAINING;

	if (sc->sc_pbusy == false) {
		// トラックバッファが空になっても、ミキサ側で処理中のデータが
		// あるかもしれない。
		// トラックミキサが動作していないときは、動作させる。
		audio_pmixer_start(sc, true);
	}

	for (;;) {
		// 終了条件判定の前に表示したい
		TRACET(track, "pid=%d.%d trkseq=%d hwseq=%d out=%d/%d/%d",
		    (int)curproc->p_pid, (int)curlwp->l_lid,
		    (int)track->seq, (int)mixer->hwseq,
		    track->outbuf.head, track->outbuf.used,
		    track->outbuf.capacity);

		if (track->outbuf.used == 0 && track->seq <= mixer->hwseq)
			break;

		error = cv_wait_sig(&mixer->draincv, sc->sc_lock);
		if (error) {
			TRACET(track, "cv_wait_sig failed %d", error);
			return error;
		}
		if (sc->sc_dying)
			return EIO;
	}

	track->pstate = AUDIO_STATE_CLEAR;
	TRACET(track, "done trk_inp=%d trk_out=%d",
		(int)track->inputcounter, (int)track->outputcounter);
	return 0;
}

/*
 * SIGIO derivery for record.
 *
 * o softintr cookie for record is one per device.  Initialize on attach()
 *   and release on detach().
 * // o audio_rmixer_process で全録音トラックへの分配が終ったところで
 * //   1回ソフトウエア割り込みを要求。
 * // o ソフトウェア割り込みで、すべての async 録音トラックに対して SIGIO を
 * //   投げる。
 * // o このため
 * //  - audio_rmixer_process -> open and set FAIOASYNC -> audoi_softintr_rd
 * //    の順でオープンされたトラックについては、録音データがまだ到着していない
 * //    にも関わらずシグナルが届くことになるがこれは許容する。
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
 * // o 再生ミキサで ASYNC トラックのバッファを消費した際に lowat を下回ったら、
 * //   ソフトウェア割り込みを要求。
 * // o ソフトウェア割り込みで、このトラックに対して SIGIO を投げる。
 */

// 再生側のソフトウェア割り込みハンドラ。
// 再生ループのハードウェア割り込みハンドラで usrbuf が lowat を下回ったら
// (今は立ち下がりではなく下回っていたらだけど)、呼ばれる。
// このディスクリプタが書き込み可能になるのを待っている selinfo に対して
// notify を実行。さらにこのディスクリプタが非同期 I/O モードならシグナルを
// 発行する。
static void
audio_softintr_wr(void *cookie)
{
	struct audio_softc *sc;
	audio_file_t *file;
	audio_file_t *f;
	proc_t *p;
	pid_t pid;

	file = cookie;
	sc = file->sc;
	TRACEF(file, "called");

	mutex_enter(sc->sc_lock);
	/* Notify for select/poll.  It needs sc_lock (and not sc_intr_lock). */
	selnotify(&sc->sc_wsel, 0, NOTE_SUBMIT);

	mutex_enter(sc->sc_intr_lock);
	// 自分自身がまだ有効かどうか調べる
	pid = 0;
	SLIST_FOREACH(f, &sc->sc_files, entry) {
		if (f == file) {
			pid = file->async_audio;
			break;
		}
	}
	mutex_exit(sc->sc_intr_lock);
	if (pid != 0) {
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
// audiosetinfo: audio_hw_set_params()コール後に対して、戻り値見てない
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

// 再生・録音ミキサーを初期化します。
// 引数 mode の AUMODE_{PLAY,RECORD} のうち立っている方を初期化します。
// 戻り値は mode のうち初期化が成功したものの OR を返します。
// この関数は再生か録音トラックがいずれかでも存在する時は呼び出しては
// いけません。
// sc_lock でコールします。
static int
audio_mixerpair_init(struct audio_softc *sc, int mode,
	audio_format2_t *phwfmt, audio_format2_t *rhwfmt)
{
	char fmtstr[64];
	int blkms;
	int error;

	if ((mode & AUMODE_PLAY)) {
		if (sc->sc_pmixer) {
			audio_mixer_destroy(sc, sc->sc_pmixer);
			kmem_free(sc->sc_pmixer, sizeof(*sc->sc_pmixer));
		}
		sc->sc_pmixer = kmem_zalloc(sizeof(*sc->sc_pmixer), KM_SLEEP);
		error = audio_mixer_init(sc, AUMODE_PLAY, phwfmt);
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
			mode &= ~AUMODE_PLAY;
		}
	}
	if ((mode & AUMODE_RECORD)) {
		if (sc->sc_rmixer) {
			audio_mixer_destroy(sc, sc->sc_rmixer);
			kmem_free(sc->sc_rmixer, sizeof(*sc->sc_rmixer));
		}
		sc->sc_rmixer = kmem_zalloc(sizeof(*sc->sc_rmixer), KM_SLEEP);
		error = audio_mixer_init(sc, AUMODE_RECORD, rhwfmt);
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
			mode &= ~AUMODE_RECORD;
		}
	}

	return mode;
}

// 周波数を選択する。
// XXX アルゴリズムどうすっかね
// 48kHz、44.1kHz をサポートしてれば優先して使用。
// そうでなければとりあえず最高周波数にしとくか。
static int
audio_select_freq(const struct audio_format *fmt)
{
	int freq;
	int high;
	int low;
	int j;

	if (fmt->frequency_type == 0) {
		low = fmt->frequency[0];
		high = fmt->frequency[1];
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
		for (j = 0; j < fmt->frequency_type; j++) {
			if (fmt->frequency[j] == 48000) {
				return fmt->frequency[j];
			}
		}
		high = 0;
		for (j = 0; j < fmt->frequency_type; j++) {
			if (fmt->frequency[j] == 44100) {
				return fmt->frequency[j];
			}
			if (fmt->frequency[j] > high) {
				high = fmt->frequency[j];
			}
		}
		return high;
	}
}

// *modep (AUMODE_PLAY|RECORD) に応じて再生か録音のフォーマットを選択する。
// 選択に失敗した方のビットは落として結果を *modep  に書き戻す。
// sc_lock でコールすること。
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
/*
 * Select playback and/or recording format (depending on *modep).
 * *modep is an in-out parameter.  It indicates the direction to configure
 * as an argument and the direction configured is written back as out
 * parameter.
 * Return 0 if successful,  otherwise errno.
 */
static int
audio_hw_probe(struct audio_softc *sc, int is_indep, int *modep,
	audio_format2_t *phwfmt, audio_format2_t *rhwfmt)
{
	audio_format2_t fmt;
	int mode;
	int error = 0;

	KASSERT(mutex_owned(sc->sc_lock));

	mode = *modep;
	KASSERTMSG((mode & (AUMODE_PLAY | AUMODE_RECORD)) != 0,
	    "invalid mode = %x", mode);

	if (is_indep) {
		/* independent devices */
		if ((mode & AUMODE_PLAY) != 0) {
			error = audio_hw_probe_fmt(sc, phwfmt, AUMODE_PLAY);
			if (error)
				mode &= ~AUMODE_PLAY;
		}
		if ((mode & AUMODE_RECORD) != 0) {
			error = audio_hw_probe_fmt(sc, rhwfmt, AUMODE_RECORD);
			if (error)
				mode &= ~AUMODE_RECORD;
		}
	} else {
		/* not independent devices */
		error = audio_hw_probe_fmt(sc, &fmt, mode);
		if (error) {
			mode = 0;
		} else {
			*phwfmt = fmt;
			*rhwfmt = fmt;
		}
	}

	*modep = mode;
	return error;
}

// 再生か録音(mode) の HW フォーマットを決定する。
// sc_lock でコールすること。
static int
audio_hw_probe_fmt(struct audio_softc *sc, audio_format2_t *cand, int mode)
{

	KASSERT(mutex_owned(sc->sc_lock));

	// 分かりやすさのため、しばらくどっち使ったか表示しとく
	if (sc->hw_if->query_format) {
		aprint_normal_dev(sc->dev, "use new query_format method\n");
		return audio_hw_probe_by_format(sc, cand, mode);
	} else {
		aprint_normal_dev(sc->dev, "use old set_param method\n");
		return audio_hw_probe_by_encoding(sc, cand, mode);
	}
}

// HW フォーマットを query_format を使って決定する。
// sc_lock でコールすること。
static int
audio_hw_probe_by_format(struct audio_softc *sc, audio_format2_t *cand,
	int mode)
{
	audio_format_query_t query;
	int i;
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	// 初期値
	// enc/prec/stride は固定。(XXX 逆エンディアン対応するか?)
	// channels/frequency はいいやつを選びたい
	cand->encoding    = AUDIO_ENCODING_SLINEAR_NE;
	cand->precision   = AUDIO_INTERNAL_BITS;
	cand->stride      = AUDIO_INTERNAL_BITS;
	cand->channels    = 1;
	cand->sample_rate = 0;	// 番兵

	for (i = 0; ; i++) {
		memset(&query, 0, sizeof(query));
		query.index = i;

		error = sc->hw_if->query_format(sc->hw_hdl, &query);
		if (error == ENOENT)
			break;
		if (error)
			return error;

#if AUDIO_DEBUG >= 1
		DPRINTF(1, "fmt[%d] %c%c %s/%dbit/%dch/", i,
		    (query.fmt.mode & AUMODE_PLAY)   ? 'P' : '-',
		    (query.fmt.mode & AUMODE_RECORD) ? 'R' : '-',
		    audio_encoding_name(query.fmt.encoding),
		    query.fmt.precision,
		    query.fmt.channels);
		if (query.fmt.frequency_type == 0) {
			DPRINTF(1, "{%d-%d",
			    query.fmt.frequency[0], query.fmt.frequency[1]);
		} else {
			int j;
			for (j = 0; j < query.fmt.frequency_type; j++) {
				DPRINTF(1, "%c%d",
				    (j == 0) ? '{' : ',',
				    query.fmt.frequency[j]);
			}
		}
		DPRINTF(1, "}\n");
#endif

		if ((query.fmt.mode & mode) == 0) {
			DPRINTF(1, "fmt[%d] skip; mode not match %d\n", i,
			    mode);
			continue;
		}

		if (query.fmt.encoding != AUDIO_ENCODING_SLINEAR_NE) {
			DPRINTF(1, "fmt[%d] skip; %s\n", i,
			    audio_encoding_name(query.fmt.encoding));
			continue;
		}
		if (query.fmt.precision != AUDIO_INTERNAL_BITS ||
		    query.fmt.validbits != AUDIO_INTERNAL_BITS) {
			DPRINTF(1, "fmt[%d] skip; precision %d/%d\n", i,
			    query.fmt.validbits, query.fmt.precision);
			continue;
		}
		if (query.fmt.channels < cand->channels) {
			DPRINTF(1, "fmt[%d] skip; channels %d < %d\n", i,
			    query.fmt.channels, cand->channels);
			continue;
		}
		int freq = audio_select_freq(&query.fmt);
		// XXX うーん
		if (freq < cand->sample_rate) {
			DPRINTF(1, "fmt[%d] skip; frequency %d < %d\n", i,
			    freq, cand->sample_rate);
			continue;
		}

		// cand 更新
		cand->channels = query.fmt.channels;
		cand->sample_rate = freq;
		DPRINTF(1, "fmt[%d] cand %dch/%dHz\n", i,
		    cand->channels, cand->sample_rate);
	}

	if (cand->sample_rate == 0) {
		DPRINTF(1, "%s no fmt\n", __func__);
		return ENXIO;
	}
	DPRINTF(1, "%s selected: %dch/%dHz\n", __func__,
	    cand->channels, cand->sample_rate);
	return 0;
}

// HW フォーマットを query_format を使わずに決定する
// とても query_format を書き起こせないような古いドライバ向けなので、
// 探索パラメータはこれでよい。
// sc_lock でコールすること。
static int
audio_hw_probe_by_encoding(struct audio_softc *sc, audio_format2_t *cand,
	int mode)
{
	static u_int freqlist[] = { 48000, 44100, 22050, 11025, 8000, 4000 };
	audio_format2_t fmt;
	u_int ch;
	u_int i;
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	fmt.encoding  = AUDIO_ENCODING_SLINEAR_LE;
	fmt.precision = AUDIO_INTERNAL_BITS;
	fmt.stride    = AUDIO_INTERNAL_BITS;

	for (ch = 2; ch > 0; ch--) {
		for (i = 0; i < __arraycount(freqlist); i++) {
			fmt.channels = ch;
			fmt.sample_rate = freqlist[i];
			error = audio_hw_set_params(sc, mode, &fmt, &fmt);
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

// spec->mode で示されるどちらか一方のミキサフォーマットをセットします。
// spec->mode は AUMODE_PLAY か AUMODE_RECORD かのいずれかのみ単独で
// 指定します。
// sc_lock でコールすること。
static int
audio_set_format(struct audio_softc *sc, audio_format_spec_t *spec)
{
	audio_trackmixer_t *mixer;
	audio_format2_t phwfmt;
	audio_format2_t rhwfmt;
	int indep;
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	// XXX PLAY|RECORD 同時指定で同時に設定変えることも書式上は
	// できそうに見えるけど、途中でこけたらどうすんだ問題が
	// 面倒なので、とりあえず放置。
	if (spec->mode == AUMODE_PLAY) {
		mixer = sc->sc_pmixer;
	} else if (spec->mode == AUMODE_RECORD) {
		mixer = sc->sc_rmixer;
	} else {
		return EINVAL;
	}
	if (mixer == NULL) {
		return ENOTTY;
	}

	if (spec->encoding != AUDIO_ENCODING_SLINEAR_NE) {
		return EINVAL;
	}
	if (spec->precision != AUDIO_INTERNAL_BITS) {
		return EINVAL;
	}
	if (spec->stride != AUDIO_INTERNAL_BITS) {
		return EINVAL;
	}

	// 再生か録音の一方だけ設定する場合であっても、
	// 再生も録音も誰もいない状態でなければいけない。
	if (sc->sc_popens + sc->sc_ropens > 0) {
		return EBUSY;
	}

	// まず両方の現在値
	if (sc->sc_pmixer)
		phwfmt = sc->sc_pmixer->track_fmt;
	if (sc->sc_rmixer)
		rhwfmt = sc->sc_rmixer->track_fmt;

	// その上で指定値を上書き。
	// channels/sample_rate のみでいい。
	// spec->mode はこの時点で PLAY か RECORD のみになっている。
	indep = (audio_get_props(sc) & AUDIO_PROP_INDEPENDENT);
	if ((spec->mode == AUMODE_PLAY) || !indep) {
		phwfmt.channels    = spec->channels;
		phwfmt.sample_rate = spec->sample_rate;
	}
	if ((spec->mode == AUMODE_RECORD) || !indep) {
		rhwfmt.channels    = spec->channels;
		rhwfmt.sample_rate = spec->sample_rate;
	}

	error = audio_hw_set_params(sc, spec->mode, &phwfmt, &rhwfmt);
	if (error)
		return error;

	audio_mixerpair_init(sc, spec->mode, &phwfmt, &rhwfmt);
	return 0;
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
//	usrbuf に受け取った / usrbuf から引き渡したサンプル数。
//	サンプル数と言っているがこれはバイト数のこと。
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
	int pchanges;
	int rchanges;
	int mode;
	struct audio_info saved_ai;
	audio_format2_t saved_pfmt;
	audio_format2_t saved_rfmt;
	int error;

	KASSERT(mutex_owned(sc->sc_lock));

	pi = &ai->play;
	ri = &ai->record;
	pchanges = 0;
	rchanges = 0;

	play = file->ptrack;
	rec = file->rtrack;

#if AUDIO_DEBUG > 1
	{
		char buf[256];
		char p[64];
		int buflen;
		int plen;
#define SPRINTF(var, fmt...) do {	\
	var##len += snprintf(var + var##len, sizeof(var) - var##len, fmt); \
} while (0)

		buflen = 0;
		plen = 0;
		if (SPECIFIED(pi->encoding))
			SPRINTF(p, "/%s", audio_encoding_name(pi->encoding));
		if (SPECIFIED(pi->precision))
			SPRINTF(p, "/%dbit", pi->precision);
		if (SPECIFIED(pi->channels))
			SPRINTF(p, "/%dch", pi->channels);
		if (SPECIFIED(pi->sample_rate))
			SPRINTF(p, "/%dHz", pi->sample_rate);
		if (plen > 0)
			SPRINTF(buf, ",play.param=%s", p + 1);

		plen = 0;
		if (SPECIFIED(ri->encoding))
			SPRINTF(p, "/%s", audio_encoding_name(ri->encoding));
		if (SPECIFIED(ri->precision))
			SPRINTF(p, "/%dbit", ri->precision);
		if (SPECIFIED(ri->channels))
			SPRINTF(p, "/%dch", ri->channels);
		if (SPECIFIED(ri->sample_rate))
			SPRINTF(p, "/%dHz", ri->sample_rate);
		if (plen > 0)
			SPRINTF(buf, ",record.param=%s", p + 1);

		if (SPECIFIED(ai->mode))
			SPRINTF(buf, ",mode=%d", ai->mode);
		if (SPECIFIED(ai->hiwat))
			SPRINTF(buf, ",hiwat=%d", ai->hiwat);
		if (SPECIFIED(ai->lowat))
			SPRINTF(buf, ",lowat=%d", ai->lowat);
		if (SPECIFIED(ai->play.gain))
			SPRINTF(buf, ",play.gain=%d", ai->play.gain);
		if (SPECIFIED(ai->record.gain))
			SPRINTF(buf, ",record.gain=%d", ai->record.gain);
		if (SPECIFIED_CH(ai->play.balance))
			SPRINTF(buf, ",play.balance=%d", ai->play.balance);
		if (SPECIFIED_CH(ai->record.balance))
			SPRINTF(buf, ",record.balance=%d", ai->record.balance);
		if (SPECIFIED(ai->play.port))
			SPRINTF(buf, ",play.port=%d", ai->play.port);
		if (SPECIFIED(ai->record.port))
			SPRINTF(buf, ",record.port=%d", ai->record.port);
		if (SPECIFIED(ai->monitor_gain))
			SPRINTF(buf, ",monitor_gain=%d", ai->monitor_gain);
		if (SPECIFIED_CH(ai->play.pause))
			SPRINTF(buf, ",play.pause=%d", ai->play.pause);
		if (SPECIFIED_CH(ai->record.pause))
			SPRINTF(buf, ",record.pause=%d", ai->record.pause);

		if (buflen > 0)
			printf("%s: specified %s\n", __func__, buf + 1);
	}
#endif

	/* XXX shut up gcc */
	memset(&saved_ai, 0xff, sizeof(saved_ai));
	memset(&saved_pfmt, 0, sizeof(saved_pfmt));
	memset(&saved_rfmt, 0, sizeof(saved_rfmt));

	/* Set default value and save current parameters */
	if (play) {
		pfmt = play->usrbuf.fmt;
		saved_pfmt = play->usrbuf.fmt;
		saved_ai.play.pause = play->is_pause;
	}
	if (rec) {
		rfmt = rec->usrbuf.fmt;
		saved_rfmt = rec->usrbuf.fmt;
		saved_ai.record.pause = rec->is_pause;
	}
	saved_ai.mode = file->mode;

	/* Overwrite if specified */
	mode = file->mode;
	if (SPECIFIED(ai->mode)) {
		// ai->mode で PLAY/REC モードを変更することはできないので
		// 出来ることは実質 PLAY モード時の PLAY_ALL ビットの上げ
		// 下ろしだけになる。
		if ((file->mode & AUMODE_PLAY) != 0) {
			mode = (file->mode & (AUMODE_PLAY | AUMODE_RECORD))
			    | (ai->mode & AUMODE_PLAY_ALL);
		} else {
			mode = file->mode;
		}

		// Half duplex なら
		// 1.PLAY | REC なら PLAY として
		// 2.PLAY なら、他に REC  がいればエラー、いなければ PLAY
		// 3.REC  なら、他に PLAY がいればエラー、いなければ REC
		if ((audio_get_props(sc) & AUDIO_PROP_FULLDUPLEX) == 0) {
			// sc_files には自身が含まれる場合(ioctl)と
			// 含まれない場合(open)の両方がある。
			bool found;
			audio_file_t *f;
			/* Play takes precedence */
			if ((mode & AUMODE_PLAY) != 0) {
				found = false;
				mutex_enter(sc->sc_intr_lock);
				SLIST_FOREACH(f, &sc->sc_files, entry) {
					if (f == file)
						continue;
					if ((f->mode & AUMODE_RECORD) != 0) {
						found = true;
						break;
					}
				}
				mutex_exit(sc->sc_intr_lock);
				if (found) {
					DPRINTF(1,
					    "%s: record track already exists\n",
					    __func__);
					return ENODEV;
				}
				mode &= ~AUMODE_RECORD;
			}
			if ((mode & AUMODE_RECORD) != 0) {
				found = false;
				mutex_enter(sc->sc_intr_lock);
				SLIST_FOREACH(f, &sc->sc_files, entry) {
					if (f == file)
						continue;
					if ((f->mode & AUMODE_PLAY) != 0) {
						found = true;
						break;
					}
				}
				mutex_exit(sc->sc_intr_lock);
				if (found) {
					DPRINTF(1,
					    "%s: play track already exists\n",
					    __func__);
					return ENODEV;
				}
			}
		}
	}

	// ここから mode は変更後の希望するモード。
	if (play) {
		pchanges = audio_file_setinfo_check(&pfmt, pi);
		if (pchanges == -1) {
			DPRINTF(1, "%s: check play.params failed\n",
			    __func__);
			return EINVAL;
		}
		if (SPECIFIED(ai->mode))
			pchanges = 1;
	}
	if (rec) {
		rchanges = audio_file_setinfo_check(&rfmt, ri);
		if (rchanges == -1) {
			DPRINTF(1, "%s: check record.params failed\n",
			    __func__);
			return EINVAL;
		}
		if (SPECIFIED(ai->mode))
			rchanges = 1;
	}

	// 録音再生どちらかでもパラメータを変更する場合は一旦両方の?
	// トラックを停止。
	// XXX 該当するほうだけでいいのでは?
	if (pchanges || rchanges) {
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

	/* Set mixer parameters */
	error = audio_hw_setinfo(sc, ai, &saved_ai);
	if (error)
		goto abort1;

	/* Set to track and update sticky parameters */
	// ポーズを解除してミキサーが停止してれば再開する。
	// open -> pause -> write -> unpause の時に起きうる。
	error = 0;
	file->mode = mode;
	if (play) {
		bool start_mixer = false;

		if (SPECIFIED_CH(pi->pause)) {
			if (play->is_pause != 0 && pi->pause == 0)
				start_mixer = true;
			play->is_pause = pi->pause;
			sc->sc_ppause = pi->pause;
		}
		if (pchanges) {
			error = audio_file_setinfo_set(play, &pfmt,
			    (mode & (AUMODE_PLAY | AUMODE_PLAY_ALL)));
			if (error) {
				DPRINTF(1, "%s: set play.params failed\n",
				    __func__);
				goto abort2;
			}
			sc->sc_pparams = pfmt;
		}
		/* Change water marks after initializing the buffers. */
		if (SPECIFIED(ai->hiwat) || SPECIFIED(ai->lowat))
			audio_track_setinfo_water(play, ai);

		if (start_mixer) {
			audio_pmixer_start(sc, false);
		}
	}
	if (rec) {
		bool start_mixer = false;

		if (SPECIFIED_CH(ri->pause)) {
			if (rec->is_pause != 0 && ri->pause == 0)
				start_mixer = true;
			rec->is_pause = ri->pause;
			sc->sc_rpause = ri->pause;
		}
		if (rchanges) {
			error = audio_file_setinfo_set(rec, &rfmt,
			    (mode & AUMODE_RECORD));
			if (error) {
				DPRINTF(1, "%s: set record.params failed\n",
				    __func__);
				goto abort3;
			}
			sc->sc_rparams = rfmt;
		}

		if (start_mixer) {
			audio_rmixer_start(sc);
		}
	}

	return 0;

	/* Rollback */
abort3:
	if (error != ENOMEM) {
		rec->is_pause = saved_ai.record.pause;
		audio_file_setinfo_set(rec, &saved_rfmt,
		    (saved_ai.mode & AUMODE_RECORD));
	}
abort2:
	if (play && error != ENOMEM) {
		play->is_pause = saved_ai.play.pause;
		file->mode = saved_ai.mode;
		audio_file_setinfo_set(play, &saved_pfmt,
		    (file->mode & (AUMODE_PLAY | AUMODE_PLAY_ALL)));
		sc->sc_pparams = saved_pfmt;
		sc->sc_ppause = saved_ai.play.pause;
	}
abort1:
	audio_hw_setinfo(sc, &saved_ai, NULL);

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

// track に mode, fmt を設定する。
// mode は再生トラックなら AUMODE_PLAY{,_ALL}、録音トラックなら AUMODE_RECORD
// のように該当するビットだけを渡すこと。
// 成功すれば0、失敗なら errno
// ここではロールバックしない。
static int
audio_file_setinfo_set(audio_track_t *track, audio_format2_t *fmt, int mode)
{
	int error;

	KASSERT(track);

	mutex_enter(track->mixer->sc->sc_intr_lock);
	track->in_use = true;
	mutex_exit(track->mixer->sc->sc_intr_lock);
	track->mode = mode;
	error = audio_track_set_format(track, fmt);
	mutex_enter(track->mixer->sc->sc_intr_lock);
	track->in_use = false;
	mutex_exit(track->mixer->sc->sc_intr_lock);
	if (error)
		return error;

	return 0;
}

/*
 * Change water marks for playback track if specfied.
 */
static void
audio_track_setinfo_water(audio_track_t *track, const struct audio_info *ai)
{
	u_int blks;
	u_int maxblks;
	u_int blksize;

	KASSERT(audio_track_is_playback(track));

	blksize = track->usrbuf_blksize;
	maxblks = track->usrbuf.capacity / blksize;

	if (SPECIFIED(ai->hiwat)) {
		blks = ai->hiwat;
		if (blks > maxblks)
			blks = maxblks;
		if (blks < 2)
			blks = 2;
		track->usrbuf_usedhigh = blks * blksize;
	}
	if (SPECIFIED(ai->lowat)) {
		blks = ai->lowat;
		if (blks > maxblks - 1)
			blks = maxblks - 1;
		track->usrbuf_usedlow = blks * blksize;
	}
	if (SPECIFIED(ai->hiwat) || SPECIFIED(ai->lowat)) {
		if (track->usrbuf_usedlow > track->usrbuf_usedhigh - blksize)
			track->usrbuf_usedlow = track->usrbuf_usedhigh -
			    blksize;
	}
}

// ai のうちハードウェア設定部分を担当する。
// ここで設定するのは *.port, *.gain, *.balance, monitor_gain。
// 途中でエラーが起きてもここではロールバックしない。
// oldai が指定されると newai の各パラメータ設定前の値を保存する。
/*
 * Set only hardware part from *ai.
 */
static int
audio_hw_setinfo(struct audio_softc *sc, const struct audio_info *newai,
	struct audio_info *oldai)
{
	const struct audio_prinfo *newpi;
	const struct audio_prinfo *newri;
	struct audio_prinfo *oldpi;
	struct audio_prinfo *oldri;
	int pbusy;
	int rbusy;
	u_int pgain;
	u_int rgain;
	u_char pbalance;
	u_char rbalance;
	int error;

	newpi = &newai->play;
	newri = &newai->record;
	if (oldai) {
		oldpi = &oldai->play;
		oldri = &oldai->record;
	}
	pbusy = sc->sc_pbusy;
	rbusy = sc->sc_rbusy;
	error = 0;

	// port を変更するにはミキサーをとめる必要があるようだ。
	/* It's necessary to stop the mixer to change the port. */
	if (SPECIFIED(newpi->port) || SPECIFIED(newri->port)) {
		mutex_enter(sc->sc_intr_lock);
		if (sc->sc_pbusy)
			audio_pmixer_halt(sc);
		if (sc->sc_rbusy)
			audio_rmixer_halt(sc);
		mutex_exit(sc->sc_intr_lock);
	}
	if (SPECIFIED(newpi->port)) {
		if (oldai)
			oldpi->port = au_get_port(sc, &sc->sc_outports);
		error = au_set_port(sc, &sc->sc_outports, newpi->port);
		if (error) {
			DPRINTF(1, "%s: set play.port=%d failed: %d\n",
			    __func__, newpi->port, error);
			goto abort;
		}
	}
	if (SPECIFIED(newri->port)) {
		if (oldai)
			oldri->port = au_get_port(sc, &sc->sc_inports);
		error = au_set_port(sc, &sc->sc_inports, newri->port);
		if (error) {
			DPRINTF(1, "%s: set record.port=%d failed: %d\n",
			    __func__, newri->port, error);
			goto abort;
		}
	}

	/*
	 * On the other hand, it's not necessary to stop the mixer to change
	 * gain, balance and monitor_gain.
	 */
	/* Backup play.{gain,balance} */
	if (SPECIFIED(newpi->gain) || SPECIFIED_CH(newpi->balance)) {
		au_get_gain(sc, &sc->sc_outports, &pgain, &pbalance);
		if (oldai) {
			oldpi->gain = pgain;
			oldpi->balance = pbalance;
		}
	}
	/* Backup record.{gain,balance} */
	if (SPECIFIED(newri->gain) || SPECIFIED_CH(newri->balance)) {
		au_get_gain(sc, &sc->sc_inports, &rgain, &rbalance);
		if (oldai) {
			oldri->gain = rgain;
			oldri->balance = rbalance;
		}
	}
	if (SPECIFIED(newpi->gain)) {
		error = au_set_gain(sc, &sc->sc_outports,
		    newpi->gain, pbalance);
		if (error) {
			DPRINTF(1, "%s: set play.gain=%d failed: %d\n",
			    __func__, newpi->gain, error);
			goto abort;
		}
	}
	if (SPECIFIED(newri->gain)) {
		error = au_set_gain(sc, &sc->sc_inports,
		    newri->gain, rbalance);
		if (error) {
			DPRINTF(1, "%s: set record.gain=%d failed: %d\n",
			    __func__, newri->gain, error);
			goto abort;
		}
	}
	if (SPECIFIED_CH(newpi->balance)) {
		error = au_set_gain(sc, &sc->sc_outports,
		    pgain, newpi->balance);
		if (error) {
			DPRINTF(1, "%s: set play.balance=%d failed: %d\n",
			    __func__, newpi->balance, error);
			goto abort;
		}
	}
	if (SPECIFIED_CH(newri->balance)) {
		error = au_set_gain(sc, &sc->sc_inports,
		    rgain, newri->balance);
		if (error) {
			DPRINTF(1, "%s: set record.balance=%d failed: %d\n",
			    __func__, newri->balance, error);
			goto abort;
		}
	}

	if (SPECIFIED(newai->monitor_gain) && sc->sc_monitor_port != -1) {
		if (oldai)
			oldai->monitor_gain = au_get_monitor_gain(sc);
		error = au_set_monitor_gain(sc, newai->monitor_gain);
		if (error) {
			DPRINTF(1, "%s: set monitor_gain=%d failed: %d\n",
			    __func__, newai->monitor_gain, error);
			goto abort;
		}
	}

	// XXX hw の変更はなくていいのでは
	//sc->sc_ai = *ai;

	error = 0;
abort:
	/* Restart the mixer if necessary */
	// XXX pmixer_start は false でいいんだろうか
	if (pbusy)
		audio_pmixer_start(sc, false);
	if (rbusy)
		audio_rmixer_start(sc);

	return error;
}

// HW に対する set_param あたり。
// setmode に PLAY_ALL は立っていない。
// indepでないデバイスなら
//  - pp, rp は同じ値がすでにセットされている。
// pp, rp は in/out parameter ではなくす。
// hw->set_params は値を変更できない。(無視する)
// sc_lock でコールすること。
static int
audio_hw_set_params(struct audio_softc *sc, int setmode,
	audio_format2_t *phwfmt, audio_format2_t *rhwfmt)
{
	audio_params_t pp, rp;
	stream_filter_list_t pfilters, rfilters;
	audio_filter_reg_t pfilters2, rfilters2;
	int error;
	int usemode;
	bool use_set_format;

	// set_format が定義されてればそっちを使う
	use_set_format = (sc->hw_if->set_format != NULL);
	if (use_set_format)
		DPRINTF(2, "%s use_set_format\n", __func__);

	usemode = setmode;
	pp = format2_to_params(phwfmt);
	rp = format2_to_params(rhwfmt);

	if (use_set_format) {
		memset(&pfilters2, 0, sizeof(pfilters2));
		memset(&rfilters2, 0, sizeof(rfilters2));
		pfilters2.param = pp;
		rfilters2.param = rp;
	} else {
		memset(&pfilters, 0, sizeof(pfilters));
		memset(&rfilters, 0, sizeof(rfilters));
#if defined(OLD_FILTER)
		pfilters.append = stream_filter_list_append;
		pfilters.prepend = stream_filter_list_prepend;
		pfilters.set = stream_filter_list_set;
		rfilters.append = stream_filter_list_append;
		rfilters.prepend = stream_filter_list_prepend;
		rfilters.set = stream_filter_list_set;
#endif
	}

	if (use_set_format) {
		error = sc->hw_if->set_format(sc->hw_hdl, setmode,
		    &pp, &rp, &pfilters2, &rfilters2);
		if (error) {
			DPRINTF(1, "%s: set_format failed with %d\n",
			    __func__, error);
			return error;
		}
	} else {
		error = sc->hw_if->set_params(sc->hw_hdl, setmode, usemode,
		    &pp, &rp, &pfilters, &rfilters);
		if (error) {
			DPRINTF(1, "%s: set_params failed with %d\n",
			    __func__, error);
			return error;
		}
	}

	if (sc->hw_if->commit_settings) {
		error = sc->hw_if->commit_settings(sc->hw_hdl);
		if (error) {
			DPRINTF(1, "%s: commit_settings failed with %d\n",
			    __func__, error);
			return error;
		}
	}

	if (use_set_format) {
		sc->sc_xxx_pfilreg = pfilters2;
		sc->sc_xxx_rfilreg = rfilters2;
		if (pfilters2.codec)
			*phwfmt = params_to_format2(&pfilters2.param);
		if (rfilters2.codec)
			*rhwfmt = params_to_format2(&rfilters2.param);
	} else {
		*phwfmt = params_to_format2(&pp);
		*rhwfmt = params_to_format2(&rp);
	}

	return 0;
}

static int
audiogetinfo(struct audio_softc *sc, struct audio_info *ai, int need_mixerinfo,
	audio_file_t *file)
{
	struct audio_prinfo *ri, *pi;
	audio_track_t *ptrack;
	audio_track_t *rtrack;
	int gain;

	KASSERT(mutex_owned(sc->sc_lock));

	ri = &ai->record;
	pi = &ai->play;
	ptrack = file->ptrack;
	rtrack = file->rtrack;

	memset(ai, 0, sizeof(*ai));

	if (ptrack) {
		pi->sample_rate = ptrack->usrbuf.fmt.sample_rate;
		pi->channels    = ptrack->usrbuf.fmt.channels;
		pi->precision   = ptrack->usrbuf.fmt.precision;
		pi->encoding    = ptrack->usrbuf.fmt.encoding;
	} else if (ISDEVAUDIO(file->dev)) {
		pi->sample_rate = audio_default.sample_rate;
		pi->channels    = audio_default.channels;
		pi->precision   = audio_default.precision;
		pi->encoding    = audio_default.encoding;
	} else {
		pi->sample_rate = sc->sc_pparams.sample_rate;
		pi->channels    = sc->sc_pparams.channels;
		pi->precision   = sc->sc_pparams.precision;
		pi->encoding    = sc->sc_pparams.encoding;
	}
	if (rtrack) {
		ri->sample_rate = rtrack->usrbuf.fmt.sample_rate;
		ri->channels    = rtrack->usrbuf.fmt.channels;
		ri->precision   = rtrack->usrbuf.fmt.precision;
		ri->encoding    = rtrack->usrbuf.fmt.encoding;
	} else if (ISDEVAUDIO(file->dev)) {
		ri->sample_rate = audio_default.sample_rate;
		ri->channels    = audio_default.channels;
		ri->precision   = audio_default.precision;
		ri->encoding    = audio_default.encoding;
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
		pi->seek = ptrack->usrbuf.used;
		pi->samples = ptrack->usrbuf_stamp;
		pi->eof = ptrack->eofcounter;
		pi->pause = ptrack->is_pause;
		pi->error = (ptrack->dropframes != 0) ? 1 : 0;
		pi->waiting = 0;		/* open never hangs */
		pi->open = 1;
		pi->active = sc->sc_pbusy;
		pi->buffer_size = ptrack->usrbuf.capacity;
	}
	if (rtrack) {
		ri->seek = rtrack->usrbuf.used;
		ri->samples = rtrack->usrbuf_stamp;
		ri->eof = 0;
		ri->pause = rtrack->is_pause;
		ri->error = (rtrack->dropframes != 0) ? 1 : 0;
		ri->waiting = 0;		/* open never hangs */
		ri->open = 1;
		ri->active = sc->sc_rbusy;
		ri->buffer_size = rtrack->usrbuf.capacity;
	}

	// XXX 再生と録音でチャンネル数が違う場合があるので
	// ブロックサイズは同じにはならないのだが、
	// もう仕方ないので異なる場合は再生側で代表するか?
	audio_track_t *track;
	// ptrack があれば ptrack、なければ rtrack、それもなければ何もしない
	track = ptrack ?: rtrack;
	if (track) {
		ai->blocksize = track->usrbuf_blksize;
		ai->hiwat = track->usrbuf_usedhigh / track->usrbuf_blksize;
		ai->lowat = track->usrbuf_usedlow / track->usrbuf_blksize;
	}
	ai->mode = file->mode;

	if (need_mixerinfo) {
		pi->port = au_get_port(sc, &sc->sc_outports);
		ri->port = au_get_port(sc, &sc->sc_inports);

		pi->avail_ports = sc->sc_outports.allports;
		ri->avail_ports = sc->sc_inports.allports;

		au_get_gain(sc, &sc->sc_outports, &pi->gain, &pi->balance);
		au_get_gain(sc, &sc->sc_inports, &ri->gain, &ri->balance);

		if (sc->sc_monitor_port != -1) {
			gain = au_get_monitor_gain(sc);
			if (gain != -1)
				ai->monitor_gain = gain;
		}
	}

	return 0;
}

#if 0 /* XXX */
// 今は使っていない。AUDIO_GETHWENC とか作れば使い道はある?
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
		DPRINTF(2, "name=\"%s\" %s prec=%d flags=%d(%s)\n",
		    ae->name,
		    audio_encoding_name(ae->encoding),
		    ae->precision,
		    ae->flags,
		    ae->flags == 0 ? "native" : "EMULATED");
	}
#endif
	return error;
}
#endif /* 0 */

static int
audio_get_props(struct audio_softc *sc)
{
	const struct audio_hw_if *hw;
	int props;

	KASSERT(mutex_owned(sc->sc_lock));

	hw = sc->hw_if;
	props = hw->get_props(sc->hw_hdl);

	/*
	 * If neither playback nor capture properties are reported,
	 * assume both are supported.
	 */
	if ((props & (AUDIO_PROP_PLAYBACK|AUDIO_PROP_CAPTURE)) == 0)
		props |= (AUDIO_PROP_PLAYBACK | AUDIO_PROP_CAPTURE);

	/* MMAP is now supported by upper layer.  */
	props |= AUDIO_PROP_MMAP;

	return props;
}

/*
 * Return true if playback is configured.
 * This function can be used after audioattach.
 */
static bool
audio_can_playback(struct audio_softc *sc)
{

	return (sc->sc_pmixer != NULL);
}

/*
 * Return true if recording is configured.
 * This function can be used after audioattach.
 */
static bool
audio_can_capture(struct audio_softc *sc)
{

	return (sc->sc_rmixer != NULL);
}

/*
 * Common routines for query_format.
 */
// format[] の有効なもののうち前から afp->index 番目のエントリを取得する。
int
audio_query_format(const struct audio_format *format, int nformats,
	audio_format_query_t *afp)
{
	const struct audio_format *f;
	int idx;
	int i;

	idx = 0;
	for (i = 0; i < nformats; i++) {
		f = &format[i];
		if (!AUFMT_IS_VALID(f))
			continue;
		if (afp->index == idx) {
			afp->fmt = *f;
			return 0;
		}
		idx++;
	}
	return ENOENT;
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
		audio_pmixer_halt(sc);
	if (sc->sc_rbusy)
		audio_rmixer_halt(sc);
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
	audio_hw_setinfo(sc, &ai, NULL);
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
	n += snprintf(buf + n, bufsize - n, "%s",
	    audio_encoding_name(fmt->encoding));
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

#ifdef DIAGNOSTIC
void
audio_diagnostic_format2(const char *func, const audio_format2_t *fmt)
{

	KASSERTMSG(fmt, "%s: fmt == NULL", func);

	// XXX:この条件どうするか検討 (MSM6258)
	if (fmt->encoding == AUDIO_ENCODING_ADPCM) {
		KASSERTMSG(fmt->stride == 4,
		    "%s: stride(%d) is invalid", func, fmt->stride);
	} else {
		KASSERTMSG(fmt->stride % NBBY == 0,
		    "%s: stride(%d) is invalid", func, fmt->stride);
	}
	KASSERTMSG(fmt->precision <= fmt->stride,
	    "%s: precision(%d) <= stride(%d)",
	    func, fmt->precision, fmt->stride);
	KASSERTMSG(1 <= fmt->channels && fmt->channels <= AUDIO_MAX_CHANNELS,
	    "%s: channels(%d) is out of range",
	    func, fmt->channels);

	/* XXX: No check for encoding */
}

void
audio_diagnostic_filter_arg(const char *func, const audio_filter_arg_t *arg)
{

	KASSERT(arg != NULL);
	KASSERT(arg->src != NULL);
	KASSERT(arg->dst != NULL);
	DIAGNOSTIC_format2(arg->srcfmt);
	DIAGNOSTIC_format2(arg->dstfmt);
	KASSERTMSG(arg->count > 0,
	    "%s: count(%d) is out of range", func, arg->count);
}

void
audio_diagnostic_ring(const char *func, const audio_ring_t *ring)
{

	KASSERTMSG(ring, "%s: ring == NULL", func);
	DIAGNOSTIC_format2(&ring->fmt);
	KASSERTMSG(0 <= ring->capacity && ring->capacity < INT_MAX / 2,
	    "%s: capacity(%d) is out of range", func, ring->capacity);
	KASSERTMSG(0 <= ring->used && ring->used <= ring->capacity,
	    "%s: used(%d) is out of range (capacity:%d)",
	    func, ring->used, ring->capacity);
	if (ring->capacity == 0) {
		KASSERTMSG(ring->mem == NULL,
		    "%s: capacity == 0 but mem != NULL", func);
	} else {
		KASSERTMSG(ring->mem != NULL,
		    "%s: capacity != 0 but mem == NULL", func);
		KASSERTMSG(0 <= ring->head && ring->head < ring->capacity,
		    "%s: head(%d) is out of range (capacity:%d)",
		    func, ring->head, ring->capacity);
	}
}
#endif /* DIAGNOSTIC */


/*
 * Mixer driver
 */
int
mixer_open(dev_t dev, struct audio_softc *sc, int flags, int ifmt,
	struct lwp *l, struct file **nfp)
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

// x gain の範囲チェックは誰がしてるんだ
// x suspend 中に uaudio が消えるケースはまた後で
// o monitor_gain の get/set は commit してもいいかも
// x pad(4) がリソースリークしてる
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
