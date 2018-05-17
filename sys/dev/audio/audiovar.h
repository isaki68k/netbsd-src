// vi:set ts=8:
/*	$NetBSD$	*/

/*-
 * Copyright (c) 2002 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by TAMURA Kent
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
 *
 *	From: Header: audiovar.h,v 1.3 93/07/18 14:07:25 mccanne Exp  (LBL)
 */

#ifndef _SYS_DEV_AUDIOVAR2_H_
#define _SYS_DEV_AUDIOVAR2_H_

#if defined(_KERNEL)
#include <sys/condvar.h>
#include <sys/proc.h>
#include <sys/queue.h>

#include <dev/audio_if.h>
#include <dev/auconv.h>
#include <dev/audio/aufilter.h>
#else
#include <stdint.h>
#include <stdbool.h>
#include "compat.h"
#include "userland.h"
#include "aufilter.h"
#endif // _KERNEL

// ユーザランドフォーマットとして [US]Linear24/24 をサポートします。
//#define AUDIO_SUPPORT_LINEAR24

// サポートする最大/最小周波数。
// 最小は、実用的に意味があるかはともかく 4kHz 未満をセットできる骨董品も
// 中にはあることを考慮してこのくらいまでは許してやろう。
// ちなみに OpenBSD は下限 4000Hz で現状妥当な数字だと思う。
// 最大は 384000 (hdaudio規格?) でもいいけど確認できないので、とりあえず
// このくらいでたちまち勘弁してもらえないか。
#define AUDIO_MIN_FREQUENCY (1000)
#define AUDIO_MAX_FREQUENCY (192000)

#if BYTE_ORDER == LITTLE_ENDIAN
#define AUDIO_ENCODING_SLINEAR_NE AUDIO_ENCODING_SLINEAR_LE
#define AUDIO_ENCODING_ULINEAR_NE AUDIO_ENCODING_ULINEAR_LE
#define AUDIO_ENCODING_SLINEAR_OE AUDIO_ENCODING_SLINEAR_BE
#define AUDIO_ENCODING_ULINEAR_OE AUDIO_ENCODING_ULINEAR_BE
#else
#define AUDIO_ENCODING_SLINEAR_NE AUDIO_ENCODING_SLINEAR_BE
#define AUDIO_ENCODING_ULINEAR_NE AUDIO_ENCODING_ULINEAR_BE
#define AUDIO_ENCODING_SLINEAR_OE AUDIO_ENCODING_SLINEAR_LE
#define AUDIO_ENCODING_ULINEAR_OE AUDIO_ENCODING_ULINEAR_LE
#endif

#define AUOPEN_READ	0x01
#define AUOPEN_WRITE	0x02

/* Interfaces for audiobell. */
int audiobellopen(dev_t, int, int, struct lwp *, struct file **);
int audiobellclose(struct file *);
int audiobellwrite(struct file *, off_t *, struct uio *, kauth_cred_t, int);
int audiobellioctl(struct file *, u_long, void *);

typedef struct audio_trackmixer audio_trackmixer_t;

/* ring buffer */
typedef struct {
	audio_format2_t fmt;	/* format */
	int  capacity;		/* capacity by frame */
	int  head;		/* head position in frame */
	int  used;		/* used frame count */
	void *mem;		/* sample ptr */
} audio_ring_t;

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

	int sc_blk_ms;			/* blocksize in msec */
	audio_trackmixer_t *sc_pmixer;	/* null if play not supported by hw */
	audio_trackmixer_t *sc_rmixer;	/* null if rec not supported by hw */

	audio_filter_reg_t sc_xxx_pfilreg;
	audio_filter_reg_t sc_xxx_rfilreg;

	int sc_popens;
	int sc_ropens;
	bool			sc_pbusy;	/* output DMA in progress */
	bool			sc_rbusy;	/* input DMA in progress */

	// この4つが /dev/sound で引き継がれる non-volatile パラメータ
	audio_format2_t sc_pparams;	/* play encoding parameters */
	audio_format2_t sc_rparams;	/* record encoding parameters */
	bool 		sc_ppause;
	bool		sc_rpause;

	struct audio_info sc_ai;	/* recent info for /dev/sound */

	struct	selinfo sc_wsel; /* write selector */
	struct	selinfo sc_rsel; /* read selector */
	void		*sc_sih_rd;
	struct	mixer_asyncs {
		struct mixer_asyncs *next;
		pid_t	pid;
	} *sc_async_mixer;  /* processes who want mixer SIGIO */

	/* Locks and sleep channels for reading, writing and draining. */
	kmutex_t	*sc_intr_lock;
	kmutex_t	*sc_lock;
	bool		sc_dying;

	kauth_cred_t sc_cred;

	struct sysctllog *sc_log;

	mixer_ctrl_t	*sc_mixer_state;
	int		sc_nmixer_states;
	int		sc_static_nmixer_states;
	struct au_mixer_ports sc_inports;
	struct au_mixer_ports sc_outports;
	int		sc_monitor_port;
	u_int	sc_lastgain;
};

#ifdef DIAGNOSTIC
#define DIAGNOSTIC_filter_arg(arg) audio_diagnostic_filter_arg(__func__, (arg))
#define DIAGNOSTIC_format2(fmt)	audio_diagnostic_format2(__func__, (fmt))
extern void audio_diagnostic_filter_arg(const char *,
	const audio_filter_arg_t *);
extern void audio_diagnostic_format2(const char *, const audio_format2_t *);
#else
#define DIAGNOSTIC_filter_arg(arg)
#define DIAGNOSTIC_format2(fmt)
#endif

/*
 * Return true if 'fmt' is the internal format.
 * It does not check for frequency and number of channels.
 */
static __inline bool
audio_format2_is_internal(const audio_format2_t *fmt)
{

	DIAGNOSTIC_format2(fmt);

	if (fmt->encoding != AUDIO_ENCODING_SLINEAR_NE)
		return false;
	if (fmt->precision != AUDIO_INTERNAL_BITS)
		return false;
	if (fmt->stride != AUDIO_INTERNAL_BITS)
		return false;
	return true;
}

/*
 * Return true if fmt's encoding is one of LINEAR.
 */
static __inline bool
audio_format2_is_linear(const audio_format2_t *fmt)
{
	return (fmt->encoding == AUDIO_ENCODING_SLINEAR_LE)
	    || (fmt->encoding == AUDIO_ENCODING_SLINEAR_BE)
	    || (fmt->encoding == AUDIO_ENCODING_ULINEAR_LE)
	    || (fmt->encoding == AUDIO_ENCODING_ULINEAR_BE);
}

/*
 * Return true if fmt's encoding is one of SLINEAR.
 */
static __inline bool
audio_format2_is_signed(const audio_format2_t *fmt)
{
	return (fmt->encoding == AUDIO_ENCODING_SLINEAR_LE)
	    || (fmt->encoding == AUDIO_ENCODING_SLINEAR_BE);
}

/*
 * Return fmt's endian as LITTLE_ENDIAN or BIG_ENDIAN.
 */
static __inline int
audio_format2_endian(const audio_format2_t *fmt)
{
	if (fmt->stride == 8) {
		/* HOST ENDIAN */
		return BYTE_ORDER;
	}

	if (fmt->encoding == AUDIO_ENCODING_SLINEAR_LE ||
	    fmt->encoding == AUDIO_ENCODING_ULINEAR_LE) {
		return LITTLE_ENDIAN;
	}
	if (fmt->encoding == AUDIO_ENCODING_SLINEAR_BE ||
	    fmt->encoding == AUDIO_ENCODING_ULINEAR_BE) {
		return BIG_ENDIAN;
	}
	return BYTE_ORDER;
}

#endif /* _SYS_DEV_AUDIOVAR2_H_ */
