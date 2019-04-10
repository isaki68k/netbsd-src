/*	$NetBSD: vsaudio.c,v 1.4 2019/04/08 14:48:33 isaki Exp $	*/
/*	$OpenBSD: vsaudio.c,v 1.4 2013/05/15 21:21:11 ratchov Exp $	*/

/*
 * Copyright (c) 2011 Miodrag Vallat.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
/*
 * Copyright (c) 1995 Rolf Grossmann
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
 *      This product includes software developed by Rolf Grossmann.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Audio backend for the VAXstation 4000 AMD79C30 audio chip.
 * Currently working in pseudo-DMA mode; DMA operation may be possible and
 * needs to be investigated.
 */
/*
 * Although he did not claim copyright for his work, this code owes a lot
 * to Blaz Antonic <blaz.antonic@siol.net> who figured out a working
 * interrupt triggering routine in vsaudio_match().
 */
/*
 * Ported to NetBSD, from OpenBSD, by Björn Johannesson (rherdware@yahoo.com)
 * in December 2014
 */

#include "audio.h"
#if NAUDIO > 0

#include <sys/errno.h>
#include <sys/evcnt.h>
#include <sys/intr.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>

#include <machine/cpu.h>
#include <machine/sid.h>
#include <machine/scb.h>
#include <machine/vsbus.h>

#include <sys/audioio.h>
#include <dev/audio_if.h>
#include <dev/auconv.h>

#include <dev/ic/am7930reg.h>
#include <dev/ic/am7930var.h>

#ifdef AUDIO_DEBUG
#define DPRINTF(x)	if (am7930debug) printf x
#define DPRINTFN(n,x)	if (am7930debug>(n)) printf x
#else
#define DPRINTF(x)
#define DPRINTFN(n,x)
#endif  /* AUDIO_DEBUG */

/* physical addresses of the AM79C30 chip */
#define VSAUDIO_CSR			0x200d0000
#define VSAUDIO_CSR_KA49		0x26800000

/* pdma state */
struct auio {
	bus_space_tag_t		au_bt;	/* bus tag */
	bus_space_handle_t	au_bh;	/* handle to chip registers */

	uint8_t		*au_rstart;	/* start of record buffer */
	uint8_t		*au_rdata;	/* record data pointer */
	uint8_t		*au_rend;	/* end of record buffer */
	uint8_t		*au_rblkend;	/* end of record block */
	uint		au_rblksize;	/* record block size */
	uint8_t		*au_pstart;	/* start of play buffer */
	uint8_t		*au_pdata;	/* play data pointer */
	uint8_t		*au_pend;	/* end of play buffer */
	uint8_t		*au_pblkend;	/* end of play block */
	uint		au_pblksize;	/* play block size */
	struct evcnt	au_intrcnt;	/* statistics */
};

struct vsaudio_softc {
	struct am7930_softc sc_am7930;	/* glue to MI code */
	bus_space_tag_t sc_bt;		/* bus cookie */
	bus_space_handle_t sc_bh;	/* device registers */

	void	(*sc_rintr)(void*);	/* input completion intr handler */
	void	*sc_rarg;		/* arg for sc_rintr() */
	void	(*sc_pintr)(void*);	/* output completion intr handler */
	void	*sc_parg;		/* arg for sc_pintr() */

	int	sc_rintr_pending;
	int	sc_pintr_pending;

	struct	auio sc_au;		/* recv and xmit buffers, etc */
#define sc_intrcnt sc_au.au_intrcnt	/* statistics */
	void	*sc_sicookie;		/* softint(9) cookie */
	int	sc_cvec;
};

static int vsaudio_match(struct device *parent, struct cfdata *match, void *);
static void vsaudio_attach(device_t parent, device_t self, void *);

CFATTACH_DECL_NEW(vsaudio, sizeof(struct vsaudio_softc), vsaudio_match,
		vsaudio_attach, NULL, NULL);

/*
 * Hardware access routines for the MI code
 */
uint8_t	vsaudio_codec_iread(struct am7930_softc *, int);
uint16_t	vsaudio_codec_iread16(struct am7930_softc *, int);
uint8_t	vsaudio_codec_dread(struct vsaudio_softc *, int);
void	vsaudio_codec_iwrite(struct am7930_softc *, int, uint8_t);
void	vsaudio_codec_iwrite16(struct am7930_softc *, int, uint16_t);
void	vsaudio_codec_dwrite(struct vsaudio_softc *, int, uint8_t);

/*
static stream_filter_factory_t vsaudio_output_conv;
static stream_filter_factory_t vsaudio_input_conv;
static int vsaudio_output_conv_fetch_to(struct audio_softc *,
		stream_fetcher_t *, audio_stream_t *, int);
static int vsaudio_input_conv_fetch_to(struct audio_softc *,
		stream_fetcher_t *, audio_stream_t *, int);
		*/

struct am7930_glue vsaudio_glue = {
	vsaudio_codec_iread,
	vsaudio_codec_iwrite,
	vsaudio_codec_iread16,
	vsaudio_codec_iwrite16,
#if !defined(AUDIO2)
	0,
	/*vsaudio_input_conv*/0,
	/*vsaudio_output_conv*/0,
#endif
};

/*
 * Interface to the MI audio layer.
 */
int	vsaudio_open(void *, int);
void	vsaudio_close(void *);
int	vsaudio_trigger_output(void *, void *, void *, int, void (*)(void *),
		void *, const audio_params_t *);
int	vsaudio_trigger_input(void *, void *, void *, int, void (*)(void *),
		void *, const audio_params_t *);
int	vsaudio_halt_output(void *);
int	vsaudio_halt_input(void *);
int	vsaudio_getdev(void *, struct audio_device *);

struct audio_hw_if vsaudio_hw_if = {
	.open			= vsaudio_open,
	.close			= vsaudio_close,
#if defined(AUDIO2)
	.query_format		= am7930_query_format,
	.set_format		= am7930_set_format,
#else
	.query_encoding		= am7930_query_encoding,
	.set_params		= am7930_set_params,
	.round_blocksize	= am7930_round_blocksize,
#endif
	.commit_settings	= am7930_commit_settings,
	.trigger_output		= vsaudio_trigger_output,
	.trigger_input		= vsaudio_trigger_input,
	.halt_output		= vsaudio_halt_output,
	.halt_input		= vsaudio_halt_input,
	.getdev			= vsaudio_getdev,
	.set_port		= am7930_set_port,
	.get_port		= am7930_get_port,
	.query_devinfo		= am7930_query_devinfo,
	.get_props		= am7930_get_props,
	.get_locks		= am7930_get_locks,
};


struct audio_device vsaudio_device = {
	"am7930",
	"x",
	"vsaudio"
};

void	vsaudio_hwintr(void *);
void	vsaudio_swintr(void *);


static int
vsaudio_match(struct device *parent, struct cfdata *match, void *aux)
{
	struct vsbus_softc *sc  __attribute__((__unused__)) = device_private(parent);
	struct vsbus_attach_args *va = aux;
	volatile uint32_t *regs;
	int i;

	switch (vax_boardtype) {
#if defined(VAX_BTYP_46) || defined(VAX_BTYP_48)
	case VAX_BTYP_46:
	case VAX_BTYP_48:
		if (va->va_paddr != VSAUDIO_CSR)
			return 0;
		break;
#endif
#if defined(VAX_BTYP_49)
	case VAX_BTYP_49:
		if (va->va_paddr != VSAUDIO_CSR_KA49)
			return 0;
		break;
#endif
	default:
		return 0;
	}

	regs = (volatile uint32_t *)va->va_addr;
	regs[AM7930_DREG_CR] = AM7930_IREG_INIT;
	regs[AM7930_DREG_DR] = AM7930_INIT_PMS_ACTIVE | AM7930_INIT_INT_ENABLE;

	regs[AM7930_DREG_CR] = AM7930_IREG_MUX_MCR1;
	regs[AM7930_DREG_DR] = 0;

	regs[AM7930_DREG_CR] = AM7930_IREG_MUX_MCR2;
	regs[AM7930_DREG_DR] = 0;

	regs[AM7930_DREG_CR] = AM7930_IREG_MUX_MCR3;
	regs[AM7930_DREG_DR] = (AM7930_MCRCHAN_BB << 4) | AM7930_MCRCHAN_BA;

	regs[AM7930_DREG_CR] = AM7930_IREG_MUX_MCR4;
	regs[AM7930_DREG_DR] = AM7930_MCR4_INT_ENABLE;

	for (i = 10; i < 20; i++)
		regs[AM7930_DREG_BBTB] = i;
	delay(1000000); /* XXX too large */

	return 1;
}


static void
vsaudio_attach(device_t parent, device_t self, void *aux)
{
	struct vsbus_attach_args *va = aux;
	struct vsaudio_softc *sc = device_private(self);

	if (bus_space_map(va->va_memt, va->va_paddr, AM7930_DREG_SIZE << 2, 0,
	    &sc->sc_bh) != 0) {
		printf(": can't map registers\n");
		return;
	}
	sc->sc_bt = va->va_memt;
	sc->sc_am7930.sc_dev = device_private(self);
	sc->sc_am7930.sc_glue = &vsaudio_glue;
	am7930_init(&sc->sc_am7930, AUDIOAMD_POLL_MODE);
	sc->sc_au.au_bt = sc->sc_bt;
	sc->sc_au.au_bh = sc->sc_bh;
	scb_vecalloc(va->va_cvec, vsaudio_hwintr, sc, SCB_ISTACK,
	    &sc->sc_intrcnt);
	sc->sc_cvec = va->va_cvec;
	evcnt_attach_dynamic(&sc->sc_intrcnt, EVCNT_TYPE_INTR, NULL,
	    device_xname(self), "intr");

	sc->sc_sicookie = softint_establish(SOFTINT_SERIAL,
	    &vsaudio_swintr, sc);
	if (sc->sc_sicookie == NULL) {
		aprint_normal("\n%s: cannot establish software interrupt\n",
		    device_xname(self));
		return;
	}

	aprint_normal("\n");
	audio_attach_mi(&vsaudio_hw_if, sc, self);

}

int
vsaudio_open(void *addr, int flags)
{
	struct vsaudio_softc *sc = addr;

	/* reset pdma state */
	sc->sc_rintr = NULL;
	sc->sc_rarg = 0;
	sc->sc_rintr_pending = 0;
	sc->sc_pintr = NULL;
	sc->sc_parg = 0;
	sc->sc_pintr_pending = 0;

	return 0;
}

void
vsaudio_close(void *addr)
{
	struct vsaudio_softc *sc = addr;

	vsaudio_halt_input(sc);
	vsaudio_halt_output(sc);
}

int
vsaudio_trigger_output(void *addr, void *start, void *end, int blksize,
    void (*intr)(void *), void *arg, const audio_params_t *params)
{
	struct vsaudio_softc *sc = addr;

	DPRINTFN(1, ("sa_trigger_output: blksize=%d %p (%p)\n",
	    blksize, intr, arg));

	sc->sc_pintr = intr;
	sc->sc_parg = arg;
	sc->sc_au.au_pstart = start;
	sc->sc_au.au_pend = end;
	sc->sc_au.au_pblksize = blksize;
	sc->sc_au.au_pdata = sc->sc_au.au_pstart;
	sc->sc_au.au_pblkend = sc->sc_au.au_pstart + sc->sc_au.au_pblksize;

	if (sc->sc_rintr == NULL) {
		vsaudio_codec_iwrite(&sc->sc_am7930,
		    AM7930_IREG_INIT, AM7930_INIT_PMS_ACTIVE);
		DPRINTF(("sa_start_output: started intrs.\n"));
	}
	return 0;
}

int
vsaudio_trigger_input(void *addr, void *start, void *end, int blksize,
    void (*intr)(void *), void *arg, const audio_params_t *params)
{
	struct vsaudio_softc *sc = addr;

	DPRINTFN(1, ("sa_trigger_input: blksize=%d %p (%p)\n",
	    blksize, intr, arg));

	sc->sc_rintr = intr;
	sc->sc_rarg = arg;
	sc->sc_au.au_rstart = start;
	sc->sc_au.au_rend = end;
	sc->sc_au.au_rblksize = blksize;
	sc->sc_au.au_rdata = sc->sc_au.au_rstart;
	sc->sc_au.au_rblkend = sc->sc_au.au_rstart + sc->sc_au.au_rblksize;

	if (sc->sc_pintr == NULL) {
		vsaudio_codec_iwrite(&sc->sc_am7930,
		    AM7930_IREG_INIT, AM7930_INIT_PMS_ACTIVE);
		DPRINTF(("sa_start_input: started intrs.\n"));
	}
	return 0;
}

int
vsaudio_halt_output(void *addr)
{
	struct vsaudio_softc *sc = addr;

	sc->sc_pintr = NULL;
	if (sc->sc_rintr == NULL) {
		vsaudio_codec_iwrite(&sc->sc_am7930,
		    AM7930_IREG_INIT,
		    AM7930_INIT_PMS_ACTIVE | AM7930_INIT_INT_DISABLE);
	}
	return 0;
}

int
vsaudio_halt_input(void *addr)
{
	struct vsaudio_softc *sc = addr;

	sc->sc_rintr = NULL;
	if (sc->sc_pintr == NULL) {
		vsaudio_codec_iwrite(&sc->sc_am7930,
		    AM7930_IREG_INIT,
		    AM7930_INIT_PMS_ACTIVE | AM7930_INIT_INT_DISABLE);
	}
	return 0;
}

void
vsaudio_hwintr(void *v)
{
	struct vsaudio_softc *sc;
	struct auio *au;
	int __attribute__((__unused__)) k;

	sc = v;
	au = &sc->sc_au;

	/* clear interrupt */
	k = vsaudio_codec_dread(sc, AM7930_DREG_IR);
#if 0   /* interrupt is not shared, this shouldn't happen */
	if ((k & (AM7930_IR_DTTHRSH | AM7930_IR_DRTHRSH | AM7930_IR_DSRI |
	    AM7930_IR_DERI | AM7930_IR_BBUFF)) == 0) {
		return 0;
	}
#endif
	/* receive incoming data */
	if (sc->sc_rintr) {
		*au->au_rdata++ = vsaudio_codec_dread(sc, AM7930_DREG_BBRB);
		if (au->au_rdata == au->au_rblkend) {
			if (au->au_rblkend == au->au_rend) {
				au->au_rdata = au->au_rstart;
				au->au_rblkend = au->au_rstart;
			}
			au->au_rblkend += au->au_rblksize;
			sc->sc_rintr_pending = 1;
			softint_schedule(sc->sc_sicookie);
		}
	}

	/* send outgoing data */
	if (sc->sc_pintr) {
		vsaudio_codec_dwrite(sc, AM7930_DREG_BBTB, *au->au_pdata++);
		if (au->au_pdata == au->au_pblkend) {
			if (au->au_pblkend == au->au_pend) {
				au->au_pdata = au->au_pstart;
				au->au_pblkend = au->au_pstart;
			}
			au->au_pblkend += au->au_pblksize;
			sc->sc_pintr_pending = 1;
			softint_schedule(sc->sc_sicookie);
		}
	}

	au->au_intrcnt.ev_count++;
}

void
vsaudio_swintr(void *cookie)
{
	struct vsaudio_softc *sc = cookie;

	mutex_spin_enter(&sc->sc_am7930.sc_intr_lock);
	if (sc->sc_rintr_pending) {
		sc->sc_rintr_pending = 0;
		(*sc->sc_rintr)(sc->sc_rarg);
	}
	if (sc->sc_pintr_pending) {
		sc->sc_pintr_pending = 0;
		(*sc->sc_pintr)(sc->sc_parg);
	}
	mutex_spin_exit(&sc->sc_am7930.sc_intr_lock);
}


/* indirect write */
void
vsaudio_codec_iwrite(struct am7930_softc *sc, int reg, uint8_t val)
{
	struct vsaudio_softc *vssc = (struct vsaudio_softc *)sc;

	vsaudio_codec_dwrite(vssc, AM7930_DREG_CR, reg);
	vsaudio_codec_dwrite(vssc, AM7930_DREG_DR, val);
}

void
vsaudio_codec_iwrite16(struct am7930_softc *sc, int reg, uint16_t val)
{
	struct vsaudio_softc *vssc = (struct vsaudio_softc *)sc;

	vsaudio_codec_dwrite(vssc, AM7930_DREG_CR, reg);
	vsaudio_codec_dwrite(vssc, AM7930_DREG_DR, val);
	vsaudio_codec_dwrite(vssc, AM7930_DREG_DR, val >> 8);
}

/* indirect read */
uint8_t
vsaudio_codec_iread(struct am7930_softc *sc, int reg)
{
	struct vsaudio_softc *vssc = (struct vsaudio_softc *)sc;

	vsaudio_codec_dwrite(vssc, AM7930_DREG_CR, reg);
	return vsaudio_codec_dread(vssc, AM7930_DREG_DR);
}

uint16_t
vsaudio_codec_iread16(struct am7930_softc *sc, int reg)
{
	struct vsaudio_softc *vssc = (struct vsaudio_softc *)sc;
	uint lo, hi;

	vsaudio_codec_dwrite(vssc, AM7930_DREG_CR, reg);
	lo = vsaudio_codec_dread(vssc, AM7930_DREG_DR);
	hi = vsaudio_codec_dread(vssc, AM7930_DREG_DR);
	return (hi << 8) | lo;
}

/* direct read */
uint8_t
vsaudio_codec_dread(struct vsaudio_softc *sc, int reg)
{
	return bus_space_read_1(sc->sc_bt, sc->sc_bh, reg << 2);
}

/* direct write */
void
vsaudio_codec_dwrite(struct vsaudio_softc *sc, int reg, uint8_t val)
{
	bus_space_write_1(sc->sc_bt, sc->sc_bh, reg << 2, val);
}

int
vsaudio_getdev(void *addr, struct audio_device *retp)
{
	*retp = vsaudio_device;
	return 0;
}

/*
static stream_filter_t *
vsaudio_input_conv(struct audio_softc *sc, const audio_params_t *from,
		const audio_params_t *to)
{
	return auconv_nocontext_filter_factory(vsaudio_input_conv_fetch_to);
}

static int
vsaudio_input_conv_fetch_to(struct audio_softc *sc, stream_fetcher_t *self,
		audio_stream_t *dst, int max_used)
{
	stream_filter_t *this;
	int m, err;

	this = (stream_filter_t *)self;
	if ((err = this->prev->fetch_to(sc, this->prev, this->src, max_used * 4)))
		return err;
	m = dst->end - dst->start;
	m = uimin(m, max_used);
	FILTER_LOOP_PROLOGUE(this->src, 4, dst, 1, m) {
		*d = ((*(const uint32_t *)s) >> 16) & 0xff;
	} FILTER_LOOP_EPILOGUE(this->src, dst);
	return 0;
}

static stream_filter_t *
vsaudio_output_conv(struct audio_softc *sc, const audio_params_t *from,
		const audio_params_t *to)
{
	return auconv_nocontext_filter_factory(vsaudio_output_conv_fetch_to);
}

static int
vsaudio_output_conv_fetch_to(struct audio_softc *sc, stream_fetcher_t *self,
		audio_stream_t *dst, int max_used)
{
	stream_filter_t *this;
	int m, err;

	this = (stream_filter_t *)self;
	max_used = (max_used + 3) & ~3;
	if ((err = this->prev->fetch_to(sc, this->prev, this->src, max_used / 4)))
		return err;
	m = (dst->end - dst->start) & ~3;
	m = uimin(m, max_used);
	FILTER_LOOP_PROLOGUE(this->src, 1, dst, 4, m) {
		*(uint32_t *)d = (*s << 16);
	} FILTER_LOOP_EPILOGUE(this->src, dst);
	return 0;
}
*/

#endif /* NAUDIO > 0 */
