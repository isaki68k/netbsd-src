/* $NetBSD: omrasops.c,v 1.21 2019/07/31 02:09:02 rin Exp $ */

/*-
 * Copyright (c) 2000 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Tohru Nishimura.
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

#include <sys/cdefs.h>			/* RCS ID & Copyright macro defns */

__KERNEL_RCSID(0, "$NetBSD: omrasops.c,v 1.21 2019/07/31 02:09:02 rin Exp $");

/*
 * Designed speficically for 'm68k bitorder';
 *	- most significant byte is stored at lower address,
 *	- most significant bit is displayed at left most on screen.
 * Implementation relies on;
 *	- first column is at 32bit aligned address,
 *	- font glyphs are stored in 32bit padded.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsdisplayvar.h>
#include <dev/rasops/rasops.h>

#include <arch/luna68k/dev/omrasopsvar.h>

// フォントビットマップは VRAM x=1280 の位置に置かれる
// [バイトオフセット]
#define FONTOFFSET		(1280 / 8)

#define USE_M68K_ASM	1

// gcc でコンパイラに最適化の条件を与える。
// clang 5 にはあるようだ…
#if defined(__GNUC__)
#define __assume(cond)	if (!(cond))__unreachable()
#elif defined(__clang__)
# if __has_builtin(__builtin_assume)
#  define __assume(cond)	__builtin_assume(cond)
# endif
#else
#define __assume(cond)	(void)(cond)
#endif

/* wscons emulator operations */
static void	om1_cursor(void *, int, int, int);
static void	om4_cursor(void *, int, int, int);
static int	om_mapchar(void *, int, unsigned int *);
static void	om1_putchar(void *, int, int, u_int, long);
static void	om4_putchar(void *, int, int, u_int, long);
static void	om1_copycols(void *, int, int, int, int);
static void	om4_copycols(void *, int, int, int, int);
static void	om1_copyrows(void *, int, int, int num);
static void	om4_copyrows(void *, int, int, int num);
static void	om1_erasecols(void *, int, int, int, long);
static void	om4_erasecols(void *, int, int, int, long);
static void	om1_eraserows(void *, int, int, long);
static void	om4_eraserows(void *, int, int, long);
static int	om1_allocattr(void *, int, int, int, long *);
static int	om4_allocattr(void *, int, int, int, long *);
static void	om4_unpack_attr(long, int *, int *, int *);

static int	omrasops_init(struct rasops_info *, int, int);

static void
om_fill(int, int,
	uint8_t *, int, int,
	uint32_t,
	int, int);
static void
om_fill_color(int,
	uint8_t *, int, int,
	int, int);

#if defined(VT100_SIXEL)
static void
omfb_sixel(struct rasops_info */*ri*/,
	int /*y*/, int /*x*/,
	u_int /*uc*/, long /*attr*/);
#endif

#define	ALL1BITS	(~0U)
#define	ALL0BITS	(0U)
#define	BLITWIDTH	(32)
#define	ALIGNMASK	(0x1f)
#define	BYTESDONE	(4)

// XXX
struct rowattr_t
{
	union {
		int32_t all;
		struct {
			int8_t ismulti; /* is multi color used */
			uint8_t fg;
			uint8_t bg;
			uint8_t reserved;
		};
	};
};
static struct rowattr_t rowattr[43];

static inline void
om_set_rowattr(int row, int fg, int bg)
{
	if (rowattr[row].fg == fg && rowattr[row].bg == bg)
		return;
	if (rowattr[row].ismulti)
		return;

	/* 単色のクリア状態から */
	if (rowattr[row].fg == rowattr[row].bg) {
		/* 両方いっぺんに変更されたらマルチカラー */
		if (rowattr[row].fg != fg && rowattr[row].bg != bg) {
			rowattr[row].ismulti = true;
		} else {
			/* そうでなければモノカラー */
			rowattr[row].fg = fg;
			rowattr[row].bg = bg;
		}
	} else {
		/* 色が変更されたらここ */
		rowattr[row].ismulti = true;
	}
}

static inline void
om_reset_rowattr(int row, int bg)
{
	rowattr[row].ismulti = false;
	rowattr[row].bg = bg;
	rowattr[row].fg = bg;	 /* fg sets same value */
}


/*
 * macros to handle unaligned bit copy ops.
 * See src/sys/dev/rasops/rasops_mask.h for MI version.
 * Also refer src/sys/arch/hp300/dev/maskbits.h.
 * (which was implemented for ancient src/sys/arch/hp300/dev/grf_hy.c)
 */

/* luna68k version GETBITS() that gets w bits from bit x at psrc memory */
#define	FASTGETBITS(psrc, x, w, dst)					\
	asm("bfextu %3{%1:%2},%0"					\
	    : "=d" (dst) 						\
	    : "di" (x), "di" (w), "o" (*(uint32_t *)(psrc)))

/* luna68k version PUTBITS() that puts w bits from bit x at pdst memory */
/* XXX this macro assumes (x + w) <= 32 to handle unaligned residual bits */
#define	FASTPUTBITS(src, x, w, pdst)					\
	asm("bfins %3,%0{%1:%2}"					\
	    : "+o" (*(uint32_t *)(pdst))				\
	    : "di" (x), "di" (w), "d" (src)				\
	    : "memory" );

#define	GETBITS(psrc, x, w, dst)	FASTGETBITS(psrc, x, w, dst)
#define	PUTBITS(src, x, w, pdst)	FASTPUTBITS(src, x, w, pdst)

/*
 * mask clear right
 */
/* ex.: width==3, (cycles at MC68030)
    mask filled with 1 at least width bits.
    mask=0b..1111111
     bclr	(6 cycle)
    mask=0b..1110111
     addq	(2 cycle)
    mask=0b..1111000 (clear right 3 bit)
*/
#if USE_M68K_ASM
#define FASTMASK_CLEAR_RIGHT(c_mask, c_bits)				\
	__asm volatile(							\
	"bclr	%[bits],%[mask];\n\t"					\
	"addq.l	#1,%[mask];\n\t"					\
	: [mask]"+&d"(c_mask)						\
	: [bits]"d"(c_bits)						\
	:								\
	)
#define MASK_CLEAR_RIGHT	FASTMASK_CLEAR_RIGHT
#else
#define MASK_CLEAR_RIGHT(c_mask, c_bits)				\
	c_mask = (c_mask & (1 << c_bits)) + 1
#endif



/*
 * fill rectangle
 * v は書き込み位置にかかわらず 32 bit の値をそのまま使うため、
 * 実質 ALL0BITS か ALL1BITS しか想定していない。
 */
static void
om_fill(int planemask, int rop,
	uint8_t *dstptr, int dstbitofs, int dstspan,
	uint32_t v,
	int width, int height)
{
	uint32_t mask;
	int dw;		/* 1 pass width bits */

	__assume(width > 0);
	__assume(height > 0);
	__assume(0 <= dstbitofs && dstbitofs < 32);

	omfb_setplanemask(planemask);

	int16_t h16 = height - 1;

	mask = ALL1BITS >> dstbitofs;
	dw = 32 - dstbitofs;

	/* for loop waste 4 clock */
	do {
		width -= dw;
		if (width < 0) {
			/* clear right zero bits */
			width = -width;
			MASK_CLEAR_RIGHT(mask, width);

			/* loop exit after done */
			width = 0;
		}

		omfb_setROP_curplane(rop, mask);

		{
			uint8_t *d = dstptr;
			dstptr += 4;
			int16_t h = h16;

#if USE_M68K_ASM
			asm volatile(
"om_fill_loop_h:\n\t"
			"move.l	%[v],(%[d]);\n\t"
			"add.l	%[dstspan],%[d];\n\t"
			"dbra	%[h],om_fill_loop_h;\n\t"
			: [d]"+&a"(d)
			 ,[h]"+&d"(h)
			: [v]"d"(v)
			 ,[dstspan]"r"(dstspan)
			: "memory"
			);
#else
			do {
				*d = v;
				d += dstspan;
			} while (--h >= 0);
#endif
		}

		mask = ALL1BITS;
		dw = 32;
	} while (width > 0);
}

static void
om_fill_color(int color,
	uint8_t *dstptr, int dstbitofs, int dstspan,
	int width, int height)
{
	uint32_t mask;
	int dw;		/* 1 pass width bits */

	__assume(width > 0);
	__assume(height > 0);
	__assume(omfb_planecount > 0);

	/* select all planes */
	omfb_setplanemask(omfb_planemask);

	mask = ALL1BITS >> dstbitofs;
	dw = 32 - dstbitofs;
	int16_t h16 = height - 1;
	int16_t lastplane = omfb_planecount - 1;

#define MAX_PLANES	(8)

	do {
		width -= dw;
		if (width < 0) {
			/* clear right zero bits */
			width = -width;
			MASK_CLEAR_RIGHT(mask, width);
			/* loop exit after done */
			width = 0;
		}

		{
			/* TODO: 中間ならマスクを再設定しない */
			uint32_t *ropfn = (uint32_t *)(BMAP_FN0 + OMFB_PLANEOFS * lastplane);
			int16_t plane = lastplane;
			int16_t rop;

#if !USE_M68K_ASM
			__asm volatile(
"om_fill_color_rop:\n\t"
			"btst	%[plane],%[color];\n\t"
			"seq	%[rop];\n\t"
			"andi.w	#0x3c,%[rop];\n\t"
			"move.l	%[mask],(%[ropfn],%[rop].w);\n\t"
			"suba.l	#0x40000,%[ropfn];\n\t"
			"dbra	%[plane],om_fill_color_rop;\n\t"
			: [plane]"+&d"(plane)
			 ,[ropfn]"+&a"(ropfn)
			 ,[rop]"=&d"(rop)
			: [color]"d"(color)
			 ,[mask]"g"(mask)
			: "memory"
			);
#else
			do {
				rop = (color & (1 << plane)) ? 0xff: 0;
				rop &= ROP_ONE;
				ropfn[rop] = mask;
				ropfn -= (OMFB_PLANEOFS >> 2);
			} while (--plane >= 0);
#endif
		}

		{
			uint8_t *d = dstptr;
			dstptr += 4;
			int16_t h = h16;

#if USE_M68K_ASM
			__asm volatile(
"om_fill_color_loop_h:\n\t"
			"clr.l	(%[d]);\n\t"	/* any data to write */
			"add.l	%[dstspan],%[d];\n\t"
			"dbra	%[h],om_fill_color_loop_h;\n\t"
			: [d]"+&a"(d)
			 ,[h]"+&d"(h)
			: [dstspan]"r"(dstspan)
			: "memory"
			);
#else
			do {
				*d = 0;
				d += dstspan;
			} while (--h >= 0);
#endif
		}

		mask = ALL1BITS;
		dw = 32;
	} while (width > 0);
}

static const uint8_t ropsel[] = {
	ROP_ZERO, ROP_INV1, ROP_THROUGH, ROP_ONE };

/*
 * 指定した色で文字を描画する。
 * しくみ：
 * プレーンROP を前景・背景に応じて次のように設定する。
 * fg bg  rop          result
 *  0  0  ROP_ZERO      0
 *  0  1  ROP_INV1     ~D
 *  1  0  ROP_THROUGH   D
 *  1  1  ROP_ONE       1
 * これにより1度の共通プレーン書き込みで、前景・背景で文字を描画できる。
 */
/*
 * x, y: destination Left-Top in pixel
 * width, height : source font size in pixel
 * fontptr: source pointer of fontdata
 * fontstride: y-stride of fontdata [byte]
 * fontx: font bit offset from fontptr MSB
 * heightscale: 0=等倍 else=縦倍角 [dstheight = height * (heightscale + 1)]
 * fg : foreground color
 * bg : background color
 *
 * breaks: planemask, ROP
 */
static void
omfb_putchar(
	struct rasops_info *ri,
	int x, int y,
	int width, int height,
	uint8_t *fontptr, int fontstride, int fontx, int heightscale,
	uint8_t fg, uint8_t bg)
{
	/* ROP アドレスのキャッシュ */
	static volatile uint32_t *ropaddr[OMFB_MAX_PLANECOUNT];
	static int saved_fg, saved_bg;

	uint32_t mask;
	int plane;
	int dw;		/* 1 pass width bits */
	uint8_t *dstC;
	int xh, xl;

	if (saved_fg != fg || saved_bg != bg) {
		saved_fg = fg;
		saved_bg = bg;
		/* ROP を求める */
		for (plane = 0; plane < omfb_planecount; plane++) {
			int t = (fg & 1) * 2 + (bg & 1);
			ropaddr[plane] = omfb_ROPaddr(plane, ropsel[t]);
			fg >>= 1;
			bg >>= 1;
		}
	}

	// x の下位5ビットと上位に分ける
	xh = x >> 5;
	xl = x & 0x1f;

	/* write to common plane */
	dstC = (uint8_t *)ri->ri_bits + xh * 4 + y * OMFB_STRIDE;

	/* select all plane */
	omfb_setplanemask(omfb_planemask);

	mask = ALL1BITS >> xl;
	dw = 32 - xl;

	__assume(
		omfb_planecount == 8
	 || omfb_planecount == 4
	 || omfb_planecount == 1);

	do {
		width -= dw;
		if (width < 0) {
			/* clear right zero bits */
			width = -width;
			MASK_CLEAR_RIGHT(mask, width);
			/* loop exit after done */
			width = 0;
		}

		/* putchar の場合、width ループは 1 か 2 回で毎回 mask が違う
		はずなので毎回セットしたほうがいい */
#if 0
		for (plane = 0; plane < omfb_planecount; plane++) {
			*(ropaddr[plane]) = mask;
		}
#else
		switch (omfb_planecount) {
		 case 8:
			*(ropaddr[7]) = mask;
			*(ropaddr[6]) = mask;
			*(ropaddr[5]) = mask;
			*(ropaddr[4]) = mask;
			/* FALLTHROUGH */
		 case 4:
			*(ropaddr[3]) = mask;
			*(ropaddr[2]) = mask;
			*(ropaddr[1]) = mask;
			/* FALLTHROUGH */
		 case 1:
			*(ropaddr[0]) = mask;
			break;
		}
#endif

		if (heightscale == 0) {
			uint8_t *d = dstC;
			uint8_t *f = fontptr;
			int16_t h = height - 1;
			do {
				uint32_t v;
				GETBITS(f, fontx, dw, v);
				/* no need to shift of v. masked by ROP */
				*W(d) = v;
				d += OMFB_STRIDE;
				f += fontstride;
			} while (--h >= 0);
		} else {
			uint8_t *d = dstC;
			uint8_t *f = fontptr;
			int16_t h = height - 1;
			do {
				uint32_t v;
				GETBITS(f, fontx, dw, v);
				/* no need to shift of v. masked by ROP */
				*W(d) = v;
				d += OMFB_STRIDE;
				*W(d) = v;
				d += OMFB_STRIDE;
				f += fontstride;
			} while (--h >= 0);
		}

		dstC += 4;
		fontx += dw;
		mask = ALL1BITS;
		dw = 32;
	} while (width > 0);
}

/*
 * Blit a character at the specified co-ordinates.
 */
static void
om1_putchar(void *cookie, int row, int startcol, u_int uc, long attr)
{
	struct rasops_info *ri = cookie;
	uint8_t *p;
	int scanspan, startx, height, width, align, y;
	uint32_t lmask, rmask, glyph, inverse;
	int i;
	uint8_t *fb;

	scanspan = ri->ri_stride;
	y = ri->ri_font->fontheight * row;
	startx = ri->ri_font->fontwidth * startcol;
	height = ri->ri_font->fontheight;
	fb = (uint8_t *)ri->ri_font->data +
	    (uc - ri->ri_font->firstchar) * ri->ri_fontscale;
	inverse = ((attr & 0x00000001) != 0) ? ALL1BITS : ALL0BITS;

	p = (uint8_t *)ri->ri_bits + y * scanspan + ((startx / 32) * 4);
	align = startx & ALIGNMASK;
	width = ri->ri_font->fontwidth + align;
	lmask = ALL1BITS >> align;
	rmask = ALL1BITS << (-width & ALIGNMASK);
	if (width <= BLITWIDTH) {
		lmask &= rmask;
		/* set lmask as ROP mask value, with THROUGH mode */
		((volatile uint32_t *)OMFB_ROPFUNC)[ROP_THROUGH] = lmask;

		while (height > 0) {
			glyph = 0;
			for (i = ri->ri_font->stride; i != 0; i--)
				glyph = (glyph << 8) | *fb++;
			glyph <<= (4 - ri->ri_font->stride) * NBBY;
			glyph = (glyph >> align) ^ inverse;

			*W(p) = glyph;

			p += scanspan;
			height--;
		}
		/* reset mask value */
		((volatile uint32_t *)OMFB_ROPFUNC)[ROP_THROUGH] = ALL1BITS;
	} else {
		uint8_t *q = p;
		uint32_t lhalf, rhalf;

		while (height > 0) {
			glyph = 0;
			for (i = ri->ri_font->stride; i != 0; i--)
				glyph = (glyph << 8) | *fb++;
			glyph <<= (4 - ri->ri_font->stride) * NBBY;
			lhalf = (glyph >> align) ^ inverse;
			/* set lmask as ROP mask value, with THROUGH mode */
			((volatile uint32_t *)OMFB_ROPFUNC)[ROP_THROUGH] =
			    lmask;

			*W(p) = lhalf;

			p += BYTESDONE;

			rhalf = (glyph << (BLITWIDTH - align)) ^ inverse;
			/* set rmask as ROP mask value, with THROUGH mode */
			((volatile uint32_t *)OMFB_ROPFUNC)[ROP_THROUGH] =
			    rmask;

			*W(p) = rhalf;

			p = (q += scanspan);
			height--;
		}
		/* reset mask value */
		((volatile uint32_t *)OMFB_ROPFUNC)[ROP_THROUGH] = ALL1BITS;
	}
}

static void
om4_putchar(void *cookie, int row, int startcol, u_int uc, long attr)
{
#if 0
	struct rasops_info *ri = cookie;
	uint8_t *p;
	int scanspan, startx, height, width, align, y;
	uint32_t lmask, rmask, glyph, glyphbg, fgpat, bgpat;
	uint32_t fgmask0, fgmask1, fgmask2, fgmask3;
	uint32_t bgmask0, bgmask1, bgmask2, bgmask3;
	int i, fg, bg;
	uint8_t *fb;

	scanspan = ri->ri_stride;
	y = ri->ri_font->fontheight * row;
	startx = ri->ri_font->fontwidth * startcol;
	height = ri->ri_font->fontheight;
	fb = (uint8_t *)ri->ri_font->data +
	    (uc - ri->ri_font->firstchar) * ri->ri_fontscale;
	om4_unpack_attr(attr, &fg, &bg, NULL);
	fgmask0 = (fg & 0x01) ? ALL1BITS : ALL0BITS;
	fgmask1 = (fg & 0x02) ? ALL1BITS : ALL0BITS;
	fgmask2 = (fg & 0x04) ? ALL1BITS : ALL0BITS;
	fgmask3 = (fg & 0x08) ? ALL1BITS : ALL0BITS;
	bgmask0 = (bg & 0x01) ? ALL1BITS : ALL0BITS;
	bgmask1 = (bg & 0x02) ? ALL1BITS : ALL0BITS;
	bgmask2 = (bg & 0x04) ? ALL1BITS : ALL0BITS;
	bgmask3 = (bg & 0x08) ? ALL1BITS : ALL0BITS;

	p = (uint8_t *)ri->ri_bits + y * scanspan + ((startx / 32) * 4);
	align = startx & ALIGNMASK;
	width = ri->ri_font->fontwidth + align;
	lmask = ALL1BITS >> align;
	rmask = ALL1BITS << (-width & ALIGNMASK);

	/* select all planes for later ROP function target */
	*(volatile uint32_t *)OMFB_PLANEMASK = 0xff;

	if (width <= BLITWIDTH) {
		lmask &= rmask;
		/* set lmask as ROP mask value, with THROUGH mode */
		((volatile uint32_t *)OMFB_ROPFUNC)[ROP_THROUGH] = lmask;

		while (height > 0) {
			glyph = 0;
			for (i = ri->ri_font->stride; i != 0; i--)
				glyph = (glyph << 8) | *fb++;
			glyph <<= (4 - ri->ri_font->stride) * NBBY;
			glyph = (glyph >> align);
			glyphbg = glyph ^ ALL1BITS;

			fgpat = glyph   & fgmask0;
			bgpat = glyphbg & bgmask0;
			*P0(p) = (fgpat | bgpat);
			fgpat = glyph   & fgmask1;
			bgpat = glyphbg & bgmask1;
			*P1(p) = (fgpat | bgpat);
			fgpat = glyph   & fgmask2;
			bgpat = glyphbg & bgmask2;
			*P2(p) = (fgpat | bgpat);
			fgpat = glyph   & fgmask3;
			bgpat = glyphbg & bgmask3;
			*P3(p) = (fgpat | bgpat);

			p += scanspan;
			height--;
		}
		/* reset mask value */
		((volatile uint32_t *)OMFB_ROPFUNC)[ROP_THROUGH] = ALL1BITS;
	} else {
		uint8_t *q = p;
		uint32_t lhalf, rhalf;
		uint32_t lhalfbg, rhalfbg;

		while (height > 0) {
			glyph = 0;
			for (i = ri->ri_font->stride; i != 0; i--)
				glyph = (glyph << 8) | *fb++;
			glyph <<= (4 - ri->ri_font->stride) * NBBY;
			lhalf = (glyph >> align);
			lhalfbg = lhalf ^ ALL1BITS;
			/* set lmask as ROP mask value, with THROUGH mode */
			((volatile uint32_t *)OMFB_ROPFUNC)[ROP_THROUGH] =
			    lmask;

			fgpat = lhalf   & fgmask0;
			bgpat = lhalfbg & bgmask0;
			*P0(p) = (fgpat | bgpat);
			fgpat = lhalf   & fgmask1;
			bgpat = lhalfbg & bgmask1;
			*P1(p) = (fgpat | bgpat);
			fgpat = lhalf   & fgmask2;
			bgpat = lhalfbg & bgmask2;
			*P2(p) = (fgpat | bgpat);
			fgpat = lhalf   & fgmask3;
			bgpat = lhalfbg & bgmask3;
			*P3(p) = (fgpat | bgpat);

			p += BYTESDONE;

			rhalf = (glyph << (BLITWIDTH - align));
			rhalfbg = rhalf ^ ALL1BITS;
			/* set rmask as ROP mask value, with THROUGH mode */
			((volatile uint32_t *)OMFB_ROPFUNC)[ROP_THROUGH] =
			    rmask;

			fgpat = rhalf   & fgmask0;
			bgpat = rhalfbg & bgmask0;
			*P0(p) = (fgpat | bgpat);
			fgpat = rhalf   & fgmask1;
			bgpat = rhalfbg & bgmask1;
			*P1(p) = (fgpat | bgpat);
			fgpat = rhalf   & fgmask2;
			bgpat = rhalfbg & bgmask2;
			*P2(p) = (fgpat | bgpat);
			fgpat = rhalf   & fgmask3;
			bgpat = rhalfbg & bgmask3;
			*P3(p) = (fgpat | bgpat);

			p = (q += scanspan);
			height--;
		}
		/* reset mask value */
		((volatile uint32_t *)OMFB_ROPFUNC)[ROP_THROUGH] = ALL1BITS;
	}
	/* select plane #0 only; XXX need this ? */
	*(volatile uint32_t *)OMFB_PLANEMASK = 0x01;
#else

	struct rasops_info *ri = cookie;
	int width;
	int height;
	int fg, bg;
	int x, y;
	int fontx, fonty;
	int fontstride;
	int heightscale;
	uint8_t *fb;

#if defined(VT100_SIXEL)
	if (attr & 1 /* WTATTR_SIXEL */) {
		omfb_sixel(ri, row, startcol, uc, attr);
		return;
	}
#endif

	y = ri->ri_font->fontheight * row;
	x = ri->ri_font->fontwidth * startcol;
	fontx = 0;
	heightscale = 0;

	if (uc <= 0x7f) {
		width = ri->ri_font->fontwidth;
		height = ri->ri_font->fontheight;

		fb = (uint8_t *)ri->ri_font->data +
		    (uc - ri->ri_font->firstchar) * ri->ri_fontscale;
		fontstride = ri->ri_font->stride;
	} else {
		uint8_t fonttype = *(uint8_t *)(OMFB_PLANE_0 + OMFB_PLANEOFS - 1);
		if (omfb_planecount == 1 || fonttype == 1) {
			height = 10;
			heightscale = 1;
			if (uc >= 0xcfd4) {
				// 1bpp support only JIS-1
				return;
			}
		} else if (fonttype == 4) {
			height = 20;
		} else {
			// font file not loaded
			return;
		}

		if (0x8ea1 <= uc && uc <= 0x8edf) {
			// 半角カナ
			int fontstep;
			int idx;

			x += 1;
			width = 10;
			idx = (uc - 0x8ea1);
			// 定数除算になることを期待している
			fontstep = 768 / width;
			fontx = idx % fontstep;
			fontx = fontx * width;
			// 半角カナは y=0 位置
			fonty = 0;
		} else {
			// 全角
			uint8_t H, L;
			int fontstep;
			int idx;

			x += 2;
			width = 20;

			H = uc >> 8;
			L = uc;
			if (L < 0xa1 || L == 0xff) {
				return;
			}
			L -= 0xa1;

			if (H < 0xa1) {
				return;
			}
			if (H <= 0xa8) {
				H = H - 0xa1;
			} else if (H < 0xad) {
				return;
			} else if (H <= 0xad) {
				H = H - 0xad + (0xa8 - 0xa1 + 1);
			} else if (H < 0xb0) {
				return;
			} else if (H <= 0xfc) {
				H = H - 0xb0 + (1 /* ad */) + (0xa8 - 0xa1 + 1);
			} else {
				return;
			}
			idx = (u_int)H * (0xfe - 0xa1 + 1) + L;
			// 定数除算になることを期待している
			// 1行に記録されている文字数
			fontstep = 768 / width;
			fontx = idx % fontstep;
			fonty = idx / fontstep;
			// ドット位置に変換
			fontx = fontx * width;
			// 半角カナのエリアが1行ある
			fonty += 1;
		}
		// m68k ではビットフィールド命令により、fontx をバイト位置に
		// 分離してポインタに加算する必要がない。
		fb = (uint8_t *)OMFB_PLANE_0 + FONTOFFSET
			+ fonty * height * OMFB_STRIDE;
		fontstride = OMFB_STRIDE;
	}
	om4_unpack_attr(attr, &fg, &bg, NULL);

	om_set_rowattr(row, fg, bg);

	omfb_putchar(ri, x, y, width, height,
		fb, fontstride, fontx, heightscale,
		fg, bg);

	omfb_resetplanemask_and_ROP();
#endif
}

static void
om1_erasecols(void *cookie, int row, int startcol, int ncols, long attr)
{
	struct rasops_info *ri = cookie;
	uint8_t *p;
	int scanspan, startx, height, width, align, w, y;
	uint32_t lmask, rmask, fill;

	scanspan = ri->ri_stride;
	y = ri->ri_font->fontheight * row;
	startx = ri->ri_font->fontwidth * startcol;
	height = ri->ri_font->fontheight;
	w = ri->ri_font->fontwidth * ncols;
	fill = ((attr & 0x00000001) != 0) ? ALL1BITS : ALL0BITS;

	p = (uint8_t *)ri->ri_bits + y * scanspan + ((startx / 32) * 4);
	align = startx & ALIGNMASK;
	width = w + align;
	lmask = ALL1BITS >> align;
	rmask = ALL1BITS << (-width & ALIGNMASK);
	if (width <= BLITWIDTH) {
		lmask &= rmask;
		fill  &= lmask;
		while (height > 0) {
			*P0(p) = (*P0(p) & ~lmask) | fill;
			p += scanspan;
			height--;
		}
	} else {
		uint8_t *q = p;
		while (height > 0) {
			*P0(p) = (*P0(p) & ~lmask) | (fill & lmask);
			width -= 2 * BLITWIDTH;
			while (width > 0) {
				p += BYTESDONE;
				*P0(p) = fill;
				width -= BLITWIDTH;
			}
			p += BYTESDONE;
			*P0(p) = (fill & rmask) | (*P0(p) & ~rmask);

			p = (q += scanspan);
			width = w + align;
			height--;
		}
	}
}

static void
om4_erasecols(void *cookie, int row, int startcol, int ncols, long attr)
{
#if 0
	struct rasops_info *ri = cookie;
	uint8_t *p;
	int scanspan, startx, height, width, align, w, y, fg, bg;
	uint32_t lmask, rmask, fill0, fill1, fill2, fill3;

	scanspan = ri->ri_stride;
	y = ri->ri_font->fontheight * row;
	startx = ri->ri_font->fontwidth * startcol;
	height = ri->ri_font->fontheight;
	w = ri->ri_font->fontwidth * ncols;
	om4_unpack_attr(attr, &fg, &bg, NULL);
	fill0 = ((bg & 0x01) != 0) ? ALL1BITS : ALL0BITS;
	fill1 = ((bg & 0x02) != 0) ? ALL1BITS : ALL0BITS;
	fill2 = ((bg & 0x04) != 0) ? ALL1BITS : ALL0BITS;
	fill3 = ((bg & 0x08) != 0) ? ALL1BITS : ALL0BITS;

	p = (uint8_t *)ri->ri_bits + y * scanspan + ((startx / 32) * 4);
	align = startx & ALIGNMASK;
	width = w + align;
	lmask = ALL1BITS >> align;
	rmask = ALL1BITS << (-width & ALIGNMASK);
	if (width <= BLITWIDTH) {
		lmask &= rmask;
		fill0 &= lmask;
		fill1 &= lmask;
		fill2 &= lmask;
		fill3 &= lmask;
		while (height > 0) {
			*P0(p) = (*P0(p) & ~lmask) | fill0;
			*P1(p) = (*P1(p) & ~lmask) | fill1;
			*P2(p) = (*P2(p) & ~lmask) | fill2;
			*P3(p) = (*P3(p) & ~lmask) | fill3;
			p += scanspan;
			height--;
		}
	} else {
		uint8_t *q = p;
		while (height > 0) {
			*P0(p) = (*P0(p) & ~lmask) | (fill0 & lmask);
			*P1(p) = (*P1(p) & ~lmask) | (fill1 & lmask);
			*P2(p) = (*P2(p) & ~lmask) | (fill2 & lmask);
			*P3(p) = (*P3(p) & ~lmask) | (fill3 & lmask);
			width -= 2 * BLITWIDTH;
			while (width > 0) {
				p += BYTESDONE;
				*P0(p) = fill0;
				*P1(p) = fill1;
				*P2(p) = fill2;
				*P3(p) = fill3;
				width -= BLITWIDTH;
			}
			p += BYTESDONE;
			*P0(p) = (fill0 & rmask) | (*P0(p) & ~rmask);
			*P1(p) = (fill1 & rmask) | (*P1(p) & ~rmask);
			*P2(p) = (fill2 & rmask) | (*P2(p) & ~rmask);
			*P3(p) = (fill3 & rmask) | (*P3(p) & ~rmask);

			p = (q += scanspan);
			width = w + align;
			height--;
		}
	}
#else
	struct rasops_info *ri = cookie;
	int startx;
	int width;
	int height;
	int fg, bg;
	int sh, sl;
	int y;
	int scanspan;
	uint8_t *p;

	scanspan = ri->ri_stride;
	y = ri->ri_font->fontheight * row;
	startx = ri->ri_font->fontwidth * startcol;
	width = ri->ri_font->fontwidth * ncols;
	height = ri->ri_font->fontheight;
	om4_unpack_attr(attr, &fg, &bg, NULL);
	sh = startx >> 5;
	sl = startx & 0x1f;
	p = (uint8_t *)ri->ri_bits + y * scanspan + sh * 4;

	// 本当はどうなの
	om_set_rowattr(row, fg, bg);

	if (bg == 0) {
		// om_fill のほうが効率がすこし良い
		om_fill(omfb_planemask, ROP_ZERO,
			p, sl, scanspan, 0, width, height);
	} else {
		om_fill_color(bg, p, sl, scanspan, width, height);
	}

	/* reset mask value */
	omfb_resetplanemask_and_ROP();
#endif
}

static void
om1_eraserows(void *cookie, int startrow, int nrows, long attr)
{
	struct rasops_info *ri = cookie;
	uint8_t *p, *q;
	int scanspan, starty, height, width, w;
	uint32_t rmask, fill;

	scanspan = ri->ri_stride;
	starty = ri->ri_font->fontheight * startrow;
	height = ri->ri_font->fontheight * nrows;
	w = ri->ri_emuwidth;
	fill = ((attr & 0x00000001) != 0) ? ALL1BITS : ALL0BITS;

	p = (uint8_t *)ri->ri_bits + starty * scanspan;
	width = w;
	rmask = ALL1BITS << (-width & ALIGNMASK);
	q = p;
	while (height > 0) {
		*P0(p) = fill;				/* always aligned */
		width -= 2 * BLITWIDTH;
		while (width > 0) {
			p += BYTESDONE;
			*P0(p) = fill;
			width -= BLITWIDTH;
		}
		p += BYTESDONE;
		*P0(p) = (fill & rmask) | (*P0(p) & ~rmask);
		p = (q += scanspan);
		width = w;
		height--;
	}
}

static void
om4_eraserows(void *cookie, int startrow, int nrows, long attr)
{
#if 0
	struct rasops_info *ri = cookie;
	uint8_t *p, *q;
	int scanspan, starty, height, width, w, fg, bg;
	uint32_t rmask, fill0, fill1, fill2, fill3;

	scanspan = ri->ri_stride;
	starty = ri->ri_font->fontheight * startrow;
	height = ri->ri_font->fontheight * nrows;
	w = ri->ri_emuwidth;
	om4_unpack_attr(attr, &fg, &bg, NULL);
	fill0 = ((bg & 0x01) != 0) ? ALL1BITS : ALL0BITS;
	fill1 = ((bg & 0x02) != 0) ? ALL1BITS : ALL0BITS;
	fill2 = ((bg & 0x04) != 0) ? ALL1BITS : ALL0BITS;
	fill3 = ((bg & 0x08) != 0) ? ALL1BITS : ALL0BITS;

	p = (uint8_t *)ri->ri_bits + starty * scanspan;
	width = w;
	rmask = ALL1BITS << (-width & ALIGNMASK);
	q = p;
	while (height > 0) {
		*P0(p) = fill0;				/* always aligned */
		*P1(p) = fill1;
		*P2(p) = fill2;
		*P3(p) = fill3;
		width -= 2 * BLITWIDTH;
		while (width > 0) {
			p += BYTESDONE;
			*P0(p) = fill0;
			*P1(p) = fill1;
			*P2(p) = fill2;
			*P3(p) = fill3;
			width -= BLITWIDTH;
		}
		p += BYTESDONE;
		*P0(p) = (fill0 & rmask) | (*P0(p) & ~rmask);
		*P1(p) = (fill1 & rmask) | (*P1(p) & ~rmask);
		*P2(p) = (fill2 & rmask) | (*P2(p) & ~rmask);
		*P3(p) = (fill3 & rmask) | (*P3(p) & ~rmask);
		p = (q += scanspan);
		width = w;
		height--;
	}
#else
	struct rasops_info *ri = cookie;
	int startx;
	int width;
	int height;
	int fg, bg;
	int sh, sl;
	int y;
	int scanspan;
	uint8_t *p;

	scanspan = ri->ri_stride;
	y = ri->ri_font->fontheight * startrow;
	startx = 0;
	width = ri->ri_emuwidth;
	height = ri->ri_font->fontheight * nrows;
	om4_unpack_attr(attr, &fg, &bg, NULL);
	sh = startx >> 5;
	sl = startx & 0x1f;
	p = (uint8_t *)ri->ri_bits + y * scanspan + sh * 4;

	for (int row = startrow; row < startrow + nrows; row++) {
		om_reset_rowattr(row, bg);
	}

	if (bg == 0) {
		// om_fill のほうが効率がすこし良い
		om_fill(omfb_planemask, ROP_ZERO,
			p, sl, scanspan, 0, width, height);
	} else {
		om_fill_color(bg, p, sl, scanspan, width, height);
	}
	/* reset mask value */
	omfb_resetplanemask_and_ROP();
#endif
}

static void
om1_copyrows(void *cookie, int srcrow, int dstrow, int nrows)
{
	struct rasops_info *ri = cookie;
	uint8_t *p, *q;
	int scanspan, offset, srcy, height, width, w;
	uint32_t rmask;

	scanspan = ri->ri_stride;
	height = ri->ri_font->fontheight * nrows;
	offset = (dstrow - srcrow) * scanspan * ri->ri_font->fontheight;
	srcy = ri->ri_font->fontheight * srcrow;
	if (srcrow < dstrow && srcrow + nrows > dstrow) {
		scanspan = -scanspan;
		srcy = srcy + height - 1;
	}

	p = (uint8_t *)ri->ri_bits + srcy * ri->ri_stride;
	w = ri->ri_emuwidth;
	width = w;
	rmask = ALL1BITS << (-width & ALIGNMASK);
	q = p;
	while (height > 0) {
		*P0(p + offset) = *P0(p);		/* always aligned */
		width -= 2 * BLITWIDTH;
		while (width > 0) {
			p += BYTESDONE;
			*P0(p + offset) = *P0(p);
			width -= BLITWIDTH;
		}
		p += BYTESDONE;
		*P0(p + offset) = (*P0(p) & rmask) | (*P0(p + offset) & ~rmask);

		p = (q += scanspan);
		width = w;
		height--;
	}
}

/*
 * solo plane raster copy
 * dst : destination plane pointer
 * src : source plane pointer
 *    if y-forward, src > dst, point to Left-Top.
 *    if y-backward, src < dst, point to Left-Bottom.
 * width: pixel width (must > 0)
 * height: pixel height (> 0 : forward, < 0 backward)
 * rop: rop[omfb_planecount] ROP
 * プレーンマスクとROP は破壊される
 */
static void
om_rascopy_solo(uint8_t *dst, uint8_t *src, int16_t width, int16_t height,
	uint8_t rop[])
{
	int wh;
	int16_t h;
	int16_t wloop, hloop;
	int step = OMFB_STRIDE;

	// X 方向は (An)+ のため、常に昇順方向

	// もしバックワードコピーならY は逆順になるようにする
	if (height < 0) {
		// 符号は step 側で管理するため、height は正にする
		step = -step;
		height = -height;
	}
	h = height - 1;	/* for dbra */

	// solo では2ロングワード単位の処理をする必然は無いが、
	// 対称性と高速化の両面から考えて、2ロングワード処理を行う。

	// まず2ロングワード単位の矩形を転送する
	wh = (width >> 6);
	if (wh > 0) {
		// 2ロングワード単位で転送

		int step8 = step - wh * 8;
		wh--;	/* for dbra */

		asm volatile(
		"move.w	%[h],%[hloop];\n\t"
"om_rascopy_solo_LL: ;\n\t"
		"move.w	%[wh],%[wloop];\n\t"

"om_rascopy_solo_LL_wloop: \n\t"
		"move.l	(%[src])+,(%[dst])+;\n\t"
		"move.l	(%[src])+,(%[dst])+;\n\t"
		"dbra	%[wloop],om_rascopy_solo_LL_wloop;\n\t"

		"adda.l	%[step],%[src];\n\t"
		"adda.l	%[step],%[dst];\n\t"

		"dbra	%[hloop],om_rascopy_solo_LL;\n\t"
		  /* output */
		: [src]"+&a"(src)
		 ,[dst]"+&a"(dst)
		 ,[hloop]"=&d"(hloop)
		 ,[wloop]"=&d"(wloop)
		  /* input */
		: [wh]"r"(wh)
		 ,[h]"g"(h)
		 ,[step]"r"(step8)
		: /* clobbers */
		  "memory"
		);

		if ((width & 0x3f) == 0) {
			// 転送完了
			return;
		}

		// 次の転送のために y を巻き戻す
		src -= height * step;
		dst -= height * step;
	}

	if (width & 32) {

		// 奇数ロングワードなので 1 ロングワード転送
		asm volatile(
		"move.l	%[h],%[hloop];\n\t"
"om_rascopy_solo_L: \n\t"
		"move.l	(%[src]),(%[dst]);\n\t"

		"adda.l	%[step],%[src];\n\t"
		"adda.l	%[step],%[dst];\n\t"

		"dbra	%[hloop],om_rascopy_solo_L;\n\t"
		  /* output */
		: [src]"+&a"(src)
		 ,[dst]"+&a"(dst)
		 ,[hloop]"=&d"(hloop)
		  /* input */
		: [h]"g"(h)
		 ,[step]"r"(step)
		: /* clobbers */
		  "memory"
		);

		if ((width & 0x1f) == 0) {
			// 転送完了
			return;
		}

		// 次の転送のために y を巻き戻す
		src += 4 - height * step;
		dst += 4 - height * step;
	}

	int wl = width & 0x1f;
	// ここまで来ていれば wl > 0
	{
		// 端数ビットの転送
		uint32_t mask;
		int plane;

		mask = ALL1BITS << (32 - wl);
		// ROP の状態を保持したたまマスクを設定することがハード的には
		// できないので、ここでは共通ROPは使えない。
		for (plane = 0; plane < omfb_planecount; plane++) {
			omfb_setROP(plane, rop[plane], mask);
		}

		asm volatile(
		"move.l	%[h],%[hloop];\n\t"
"om4_rascopy_solo_bit: \n\t"
		"move.l	(%[src]),(%[dst]);\n\t"

		"adda.l	%[step],%[src];\n\t"
		"adda.l	%[step],%[dst];\n\t"

		"dbra	%[hloop],om4_rascopy_solo_bit;\n\t"
		  /* output */
		: [src]"+&a"(src)
		 ,[dst]"+&a"(dst)
		 ,[hloop]"=&d"(hloop)
		  /* input */
		: [h]"g"(h)
		 ,[step]"r"(step)
		: /* clobbers */
		  "memory"
		);

		for (plane = 0; plane < omfb_planecount; plane++) {
			omfb_setROP(plane, rop[plane], ALL1BITS);
		}
	}
}


/*
 * multiple plane raster copy
 * dst0 : destination Plane0 pointer
 * src0 : source Plane0 pointer
 *    if y-forward, src0 > dst0, point to Left-Top.
 *    if y-backward, src0 < dst0, point to Left-Bottom.
 * width: pixel width (must > 0)
 * height: pixel height (> 0 : forward, < 0 backward)
 * プレーンマスクとROP は破壊される
 */
static void
om4_rascopy_multi(uint8_t *dst0, uint8_t *src0, int16_t width, int16_t height)
{
	int wh;
	int16_t h;
	int16_t wloop, hloop;
	uint8_t *dst1, *dst2, *dst3;
	int rewind;
	int step = OMFB_STRIDE;

	// X 方向は (An)+ のため、常に昇順方向

	// もしバックワードコピーならY は逆順になるようにする
	if (height < 0) {
		// 符号は step 側で管理するため、height は正にする
		step = -step;
		height = -height;
	}
	h = height - 1;	/* for dbra */

	dst1 = dst0 + OMFB_PLANEOFS;
	dst2 = dst1 + OMFB_PLANEOFS;
	dst3 = dst2 + OMFB_PLANEOFS;


	// まず2ロングワード単位の矩形を転送する
	wh = (width >> 6);
	if (wh > 0) {
		// 2ロングワード単位で転送

		int step8 = step - wh * 8;
		wh--;	/* for dbra */

		asm volatile(
		"move.w	%[h],%[hloop];\n\t"
"om4_rascopy_multi_LL: ;\n\t"
		"move.w	%[wh],%[wloop];\n\t"

"om4_rascopy_multi_LL_wloop: \n\t"
		/* fastest way for MC68030 */
		/* 命令のオーバーラップとアクセスウェイトの関係で、LUNA では
		move.l (An)+,(An)+ よりも
		move.l (An,Dn),(An,Dn) よりも
		movem.l よりも速い */
		/* ソースの (An)+ は Head 0 だけど adda は Head 2 なので、
		前の命令にライトウェイトサイクルがあって Tail が発生すると
		(An)+,.. はオーバーラップしないが adda はオーバーラップできる。 */

		"move.l	(%[src]),(%[dst0])+;\n\t"	/* P0 */
		"adda.l	%[PLANEOFS],%[src];\n\t"
		"move.l	(%[src]),(%[dst1])+;\n\t"	/* P1 */
		"adda.l	%[PLANEOFS],%[src];\n\t"
		"move.l	(%[src]),(%[dst2])+;\n\t"	/* P2 */
		"adda.l	%[PLANEOFS],%[src];\n\t"
		"move.l	(%[src]),(%[dst3])+;\n\t"	/* P3 */

		"addq.l	#4,%[src];\n\t"	// オーバーラップを期待して ()+ にしない

		"move.l	(%[src]),(%[dst3])+;\n\t"	/* P3 */
		"suba.l	%[PLANEOFS],%[src];\n\t"
		"move.l	(%[src]),(%[dst2])+;\n\t"	/* P2 */
		"suba.l	%[PLANEOFS],%[src];\n\t"
		"move.l	(%[src]),(%[dst1])+;\n\t"	/* P1 */
		"suba.l	%[PLANEOFS],%[src];\n\t"
		"move.l	(%[src])+,(%[dst0])+;\n\t"	/* P0 */

		"dbra	%[wloop],om4_rascopy_multi_LL_wloop;\n\t"

		"adda.l	%[step],%[src];\n\t"
		"adda.l	%[step],%[dst0];\n\t"
		"adda.l	%[step],%[dst1];\n\t"
		"adda.l	%[step],%[dst2];\n\t"
		"adda.l	%[step],%[dst3];\n\t"

		"dbra	%[hloop],om4_rascopy_multi_LL;\n\t"
		  /* output */
		: [src]"+&a"(src0)
		 ,[dst0]"+&a"(dst0)
		 ,[dst1]"+&a"(dst1)
		 ,[dst2]"+&a"(dst2)
		 ,[dst3]"+&a"(dst3)
		 ,[hloop]"=&d"(hloop)
		 ,[wloop]"=&d"(wloop)
		  /* input */
		: [wh]"r"(wh)
		 ,[h]"g"(h)
		 ,[PLANEOFS]"r"(OMFB_PLANEOFS)
		 ,[step]"r"(step8)
		: /* clobbers */
		  "memory"
		);

		if ((width & 0x3f) == 0) {
			// 転送完了
			return;
		}

		// 次の転送のために y を巻き戻す
		src0 -= height * step;
		dst0 -= height * step;
		dst1 -= height * step;
		dst2 -= height * step;
		dst3 -= height * step;
	}

	// rewind はプレーンの巻き戻しなので Y 順序とは関係ない
	rewind = OMFB_STRIDE - OMFB_PLANEOFS * 3;

	if (width & 32) {

		// 奇数ロングワードなので 1 ロングワード転送
		asm volatile(
		"move.l	%[h],%[hloop];\n\t"
"om4_rascopy_multi_L: \n\t"
		"move.l	(%[src]),(%[dst0]);\n\t"
		"adda.l	%[PLANEOFS],%[src];\n\t"
		"move.l	(%[src]),(%[dst1]);\n\t"
		"adda.l	%[PLANEOFS],%[src];\n\t"
		"move.l	(%[src]),(%[dst2]);\n\t"
		"adda.l	%[PLANEOFS],%[src];\n\t"
		"move.l	(%[src]),(%[dst3]);\n\t"
		"adda.l	%[rewind],%[src];\n\t"

		"adda.l	%[step],%[dst0];\n\t"
		"adda.l	%[step],%[dst1];\n\t"
		"adda.l	%[step],%[dst2];\n\t"
		"adda.l	%[step],%[dst3];\n\t"

		"dbra	%[hloop],om4_rascopy_multi_L;\n\t"
		  /* output */
		: [src]"+&a"(src0)
		 ,[dst0]"+&a"(dst0)
		 ,[dst1]"+&a"(dst1)
		 ,[dst2]"+&a"(dst2)
		 ,[dst3]"+&a"(dst3)
		 ,[hloop]"=&d"(hloop)
		  /* input */
		: [h]"g"(h)
		 ,[PLANEOFS]"r"(OMFB_PLANEOFS)
		 ,[rewind]"r"(rewind)
		 ,[step]"r"(step)
		: /* clobbers */
		  "memory"
		);

		if ((width & 0x1f) == 0) {
			// 転送完了
			return;
		}

		// 次の転送のために y を巻き戻す
		src0 += 4 - height * step;
		dst0 += 4 - height * step;
		dst1 += 4 - height * step;
		dst2 += 4 - height * step;
		dst3 += 4 - height * step;
	}
	int wl = width & 0x1f;
	// ここまで来ていれば wl > 0
	{
		// 端数ビットの転送
		uint32_t mask;

		mask = ALL1BITS << (32 - wl);
		omfb_setplanemask(omfb_planemask);
		omfb_setROP_curplane(ROP_THROUGH, mask);

		asm volatile(
		"move.l	%[h],%[hloop];\n\t"
"om4_rascopy_multi_bit: \n\t"
		"move.l	(%[src]),(%[dst0]);\n\t"
		"adda.l	%[PLANEOFS],%[src];\n\t"
		"move.l	(%[src]),(%[dst1]);\n\t"
		"adda.l	%[PLANEOFS],%[src];\n\t"
		"move.l	(%[src]),(%[dst2]);\n\t"
		"adda.l	%[PLANEOFS],%[src];\n\t"
		"move.l	(%[src]),(%[dst3]);\n\t"
		"adda.l	%[rewind],%[src];\n\t"

		"adda.l	%[step],%[dst0];\n\t"
		"adda.l	%[step],%[dst1];\n\t"
		"adda.l	%[step],%[dst2];\n\t"
		"adda.l	%[step],%[dst3];\n\t"

		: [src]"+&a"(src0)
		 ,[dst0]"+&a"(dst0)
		 ,[dst1]"+&a"(dst1)
		 ,[dst2]"+&a"(dst2)
		 ,[dst3]"+&a"(dst3)
		 ,[hloop]"=&d"(hloop)
		  /* input */
		: [h]"g"(h)
		 ,[PLANEOFS]"r"(OMFB_PLANEOFS)
		 ,[rewind]"r"(rewind)
		 ,[step]"r"(step)
		: /* clobbers */
		  "memory"
		);

		omfb_resetplanemask_and_ROP();

	}
}

static void
om4_copyrows(void *cookie, int srcrow, int dstrow, int nrows)
{
#if 0
	// dd if=32 3.50sec

	struct rasops_info *ri = cookie;
	uint8_t *p, *q;
	int scanspan, offset, srcy, height, width, w;
	uint32_t rmask;

	scanspan = ri->ri_stride;
	height = ri->ri_font->fontheight * nrows;
	offset = (dstrow - srcrow) * scanspan * ri->ri_font->fontheight;
	srcy = ri->ri_font->fontheight * srcrow;
	if (srcrow < dstrow && srcrow + nrows > dstrow) {
		scanspan = -scanspan;
		srcy = srcy + height - 1;
	}

	p = (uint8_t *)ri->ri_bits + srcy * ri->ri_stride;
	w = ri->ri_emuwidth;
	width = w;
	rmask = ALL1BITS << (-width & ALIGNMASK);
	q = p;
	while (height > 0) {
		*P0(p + offset) = *P0(p);		/* always aligned */
		*P1(p + offset) = *P1(p);
		*P2(p + offset) = *P2(p);
		*P3(p + offset) = *P3(p);
		width -= 2 * BLITWIDTH;
		while (width > 0) {
			p += BYTESDONE;
			*P0(p + offset) = *P0(p);
			*P1(p + offset) = *P1(p);
			*P2(p + offset) = *P2(p);
			*P3(p + offset) = *P3(p);
			width -= BLITWIDTH;
		}
		p += BYTESDONE;
		*P0(p + offset) = (*P0(p) & rmask) | (*P0(p + offset) & ~rmask);
		*P1(p + offset) = (*P1(p) & rmask) | (*P1(p + offset) & ~rmask);
		*P2(p + offset) = (*P2(p) & rmask) | (*P2(p + offset) & ~rmask);
		*P3(p + offset) = (*P3(p) & rmask) | (*P3(p + offset) & ~rmask);

		p = (q += scanspan);
		width = w;
		height--;
	}

#else

	// dd if=32 0.116sec

	struct rasops_info *ri = cookie;
	uint8_t *src, *dst;
	int width, rowheight;
	int ptrstep, rowstep;

	width = ri->ri_emuwidth;
	rowheight = ri->ri_font->fontheight;
	src = (uint8_t *)ri->ri_bits + srcrow * rowheight * ri->ri_stride;
	dst = (uint8_t *)ri->ri_bits + dstrow * rowheight * ri->ri_stride;

	if (nrows <= 0 || srcrow == dstrow) {
		return;
	} else if (srcrow < dstrow) {
		/* y-backward */
		// Bottom 行、Bottom ラスタを選択
		srcrow += nrows - 1;
		dstrow += nrows - 1;
		src += nrows * rowheight * ri->ri_stride - ri->ri_stride;
		dst += nrows * rowheight * ri->ri_stride - ri->ri_stride;
		rowstep = -1;
		rowheight = -rowheight;
	} else {
		/* y-forward*/
		rowstep = 1;
	}
	ptrstep = ri->ri_stride * rowheight;


	omfb_setplanemask(omfb_planemask);

	uint8_t rop[OMFB_MAX_PLANECOUNT];
	int srcplane = 0;
	int i;
	uint32_t srcplaneofs = 0;
	int r;

	while (nrows > 0) {
		r = 1;
		if (rowattr[srcrow].ismulti == false
		 && rowattr[srcrow].fg == rowattr[srcrow].bg
		 && rowattr[srcrow].all == rowattr[dstrow].all) {
			goto skip;
		}

		/* 同じ行状態にある行数を数える */
		for (; r < nrows; r++) {
			if (rowattr[srcrow + r * rowstep].all != rowattr[srcrow].all) {
				break;
			}
		}
		// この結果、r は srcrow 自身を含めた行数

		if (rowattr[srcrow].ismulti) {
			// src とdst は共通プレーンを指しているので P0 に変換
			uint8_t *src0 = src + OMFB_PLANEOFS;
			uint8_t *dst0 = dst + OMFB_PLANEOFS;
			omfb_setROP_curplane(ROP_THROUGH, ALL1BITS);
			om4_rascopy_multi(dst0, src0, width, rowheight * r);
		} else {
			uint8_t fg = rowattr[srcrow].fg;
			uint8_t bg = rowattr[srcrow].bg;
			srcplane = 0;
			/* ROP 選択のロジックは putchar と同じ */
			/* srcplane は fg が立ってて bg が立ってないプレーン */
			for (i = 0; i < omfb_planecount; i++) {
				int t = (fg & 1) * 2 + (bg & 1);
				rop[i] = ropsel[t];
				omfb_setROP(i, rop[i], ALL1BITS);
				if (t == 2) {
					srcplane = i;
				}
				fg >>= 1;
				bg >>= 1;
			}

			srcplaneofs = OMFB_PLANEOFS + srcplane * OMFB_PLANEOFS;

			uint8_t *srcP = src + srcplaneofs;
			om_rascopy_solo(dst, srcP, width, rowheight * r, rop);
		}

 skip:
		for (i = 0; i < r; i++) {
			rowattr[dstrow] = rowattr[srcrow];

			srcrow += rowstep;
			dstrow += rowstep;
			src += ptrstep;
			dst += ptrstep;
			nrows--;
		}
	}
#endif
}

static void
om1_copycols(void *cookie, int startrow, int srccol, int dstcol, int ncols)
{
	struct rasops_info *ri = cookie;
	uint8_t *sp, *dp, *sq, *dq, *basep;
	int scanspan, height, w, y, srcx, dstx;
	int sb, eb, db, sboff, full, cnt, lnum, rnum;
	uint32_t lmask, rmask, tmp;
	bool sbover;

	scanspan = ri->ri_stride;
	y = ri->ri_font->fontheight * startrow;
	srcx = ri->ri_font->fontwidth * srccol;
	dstx = ri->ri_font->fontwidth * dstcol;
	height = ri->ri_font->fontheight;
	w = ri->ri_font->fontwidth * ncols;
	basep = (uint8_t *)ri->ri_bits + y * scanspan;

	sb = srcx & ALIGNMASK;
	db = dstx & ALIGNMASK;

	if (db + w <= BLITWIDTH) {
		/* Destination is contained within a single word */
		sp = basep + (srcx / 32) * 4;
		dp = basep + (dstx / 32) * 4;

		while (height > 0) {
			GETBITS(P0(sp), sb, w, tmp);
			PUTBITS(tmp, db, w, P0(dp));
			dp += scanspan;
			sp += scanspan;
			height--;
		}
		return;
	}

	lmask = (db == 0) ? 0 : ALL1BITS >> db;
	eb = (db + w) & ALIGNMASK;
	rmask = (eb == 0) ? 0 : ALL1BITS << (32 - eb);
	lnum = (32 - db) & ALIGNMASK;
	rnum = (dstx + w) & ALIGNMASK;

	if (lmask != 0)
		full = (w - (32 - db)) / 32;
	else
		full = w / 32;

	sbover = (sb + lnum) >= 32;

	if (dstcol < srccol || srccol + ncols < dstcol) {
		/* copy forward (left-to-right) */
		sp = basep + (srcx / 32) * 4;
		dp = basep + (dstx / 32) * 4;

		if (lmask != 0) {
			sboff = sb + lnum;
			if (sboff >= 32)
				sboff -= 32;
		} else
			sboff = sb;

		sq = sp;
		dq = dp;
		while (height > 0) {
			if (lmask != 0) {
				GETBITS(P0(sp), sb, lnum, tmp);
				PUTBITS(tmp, db, lnum, P0(dp));
				dp += BYTESDONE;
				if (sbover)
					sp += BYTESDONE;
			}

			for (cnt = full; cnt; cnt--) {
				GETBITS(P0(sp), sboff, 32, tmp);
				*P0(dp) = tmp;
				sp += BYTESDONE;
				dp += BYTESDONE;
			}

			if (rmask != 0) {
				GETBITS(P0(sp), sboff, rnum, tmp);
				PUTBITS(tmp, 0, rnum, P0(dp));
			}

			sp = (sq += scanspan);
			dp = (dq += scanspan);
			height--;
		}
	} else {
		/* copy backward (right-to-left) */
		sp = basep + ((srcx + w) / 32) * 4;
		dp = basep + ((dstx + w) / 32) * 4;

		sboff = (srcx + w) & ALIGNMASK;
		sboff -= rnum;
		if (sboff < 0) {
			sp -= BYTESDONE;
			sboff += 32;
		}

		sq = sp;
		dq = dp;
		while (height > 0) {
			if (rnum != 0) {
				GETBITS(P0(sp), sboff, rnum, tmp);
				PUTBITS(tmp, 0, rnum, P0(dp));
			}

			for (cnt = full; cnt; cnt--) {
				sp -= BYTESDONE;
				dp -= BYTESDONE;
				GETBITS(P0(sp), sboff, 32, tmp);
				*P0(dp) = tmp;
			}

			if (lmask != 0) {
				if (sbover)
					sp -= BYTESDONE;
				dp -= BYTESDONE;
				GETBITS(P0(sp), sb, lnum, tmp);
				PUTBITS(tmp, db, lnum, P0(dp));
			}

			sp = (sq += scanspan);
			dp = (dq += scanspan);
			height--;
		}
	}
}

static void
om4_copycols(void *cookie, int startrow, int srccol, int dstcol, int ncols)
{
	struct rasops_info *ri = cookie;
	uint8_t *sp, *dp, *sq, *dq, *basep;
	int scanspan, height, w, y, srcx, dstx;
	int sb, eb, db, sboff, full, cnt, lnum, rnum;
	uint32_t lmask, rmask, tmp;
	bool sbover;

	scanspan = ri->ri_stride;
	y = ri->ri_font->fontheight * startrow;
	srcx = ri->ri_font->fontwidth * srccol;
	dstx = ri->ri_font->fontwidth * dstcol;
	height = ri->ri_font->fontheight;
	w = ri->ri_font->fontwidth * ncols;
	basep = (uint8_t *)ri->ri_bits + y * scanspan;

	sb = srcx & ALIGNMASK;
	db = dstx & ALIGNMASK;

	if (db + w <= BLITWIDTH) {
		/* Destination is contained within a single word */
		sp = basep + (srcx / 32) * 4;
		dp = basep + (dstx / 32) * 4;

		while (height > 0) {
			GETBITS(P0(sp), sb, w, tmp);
			PUTBITS(tmp, db, w, P0(dp));
			GETBITS(P1(sp), sb, w, tmp);
			PUTBITS(tmp, db, w, P1(dp));
			GETBITS(P2(sp), sb, w, tmp);
			PUTBITS(tmp, db, w, P2(dp));
			GETBITS(P3(sp), sb, w, tmp);
			PUTBITS(tmp, db, w, P3(dp));
			dp += scanspan;
			sp += scanspan;
			height--;
		}
		return;
	}

	lmask = (db == 0) ? 0 : ALL1BITS >> db;
	eb = (db + w) & ALIGNMASK;
	rmask = (eb == 0) ? 0 : ALL1BITS << (32 - eb);
	lnum = (32 - db) & ALIGNMASK;
	rnum = (dstx + w) & ALIGNMASK;

	if (lmask != 0)
		full = (w - (32 - db)) / 32;
	else
		full = w / 32;

	sbover = (sb + lnum) >= 32;

	if (dstcol < srccol || srccol + ncols < dstcol) {
		/* copy forward (left-to-right) */
		sp = basep + (srcx / 32) * 4;
		dp = basep + (dstx / 32) * 4;

		if (lmask != 0) {
			sboff = sb + lnum;
			if (sboff >= 32)
				sboff -= 32;
		} else
			sboff = sb;

		sq = sp;
		dq = dp;
		while (height > 0) {
			if (lmask != 0) {
				GETBITS(P0(sp), sb, lnum, tmp);
				PUTBITS(tmp, db, lnum, P0(dp));
				GETBITS(P1(sp), sb, lnum, tmp);
				PUTBITS(tmp, db, lnum, P1(dp));
				GETBITS(P2(sp), sb, lnum, tmp);
				PUTBITS(tmp, db, lnum, P2(dp));
				GETBITS(P3(sp), sb, lnum, tmp);
				PUTBITS(tmp, db, lnum, P3(dp));
				dp += BYTESDONE;
				if (sbover)
					sp += BYTESDONE;
			}

			for (cnt = full; cnt; cnt--) {
				GETBITS(P0(sp), sboff, 32, tmp);
				*P0(dp) = tmp;
				GETBITS(P1(sp), sboff, 32, tmp);
				*P1(dp) = tmp;
				GETBITS(P2(sp), sboff, 32, tmp);
				*P2(dp) = tmp;
				GETBITS(P3(sp), sboff, 32, tmp);
				*P3(dp) = tmp;
				sp += BYTESDONE;
				dp += BYTESDONE;
			}

			if (rmask != 0) {
				GETBITS(P0(sp), sboff, rnum, tmp);
				PUTBITS(tmp, 0, rnum, P0(dp));
				GETBITS(P1(sp), sboff, rnum, tmp);
				PUTBITS(tmp, 0, rnum, P1(dp));
				GETBITS(P2(sp), sboff, rnum, tmp);
				PUTBITS(tmp, 0, rnum, P2(dp));
				GETBITS(P3(sp), sboff, rnum, tmp);
				PUTBITS(tmp, 0, rnum, P3(dp));
			}

			sp = (sq += scanspan);
			dp = (dq += scanspan);
			height--;
		}
	} else {
		/* copy backward (right-to-left) */
		sp = basep + ((srcx + w) / 32) * 4;
		dp = basep + ((dstx + w) / 32) * 4;

		sboff = (srcx + w) & ALIGNMASK;
		sboff -= rnum;
		if (sboff < 0) {
			sp -= BYTESDONE;
			sboff += 32;
		}

		sq = sp;
		dq = dp;
		while (height > 0) {
			if (rnum != 0) {
				GETBITS(P0(sp), sboff, rnum, tmp);
				PUTBITS(tmp, 0, rnum, P0(dp));
				GETBITS(P1(sp), sboff, rnum, tmp);
				PUTBITS(tmp, 0, rnum, P1(dp));
				GETBITS(P2(sp), sboff, rnum, tmp);
				PUTBITS(tmp, 0, rnum, P2(dp));
				GETBITS(P3(sp), sboff, rnum, tmp);
				PUTBITS(tmp, 0, rnum, P3(dp));
			}

			for (cnt = full; cnt; cnt--) {
				sp -= BYTESDONE;
				dp -= BYTESDONE;
				GETBITS(P0(sp), sboff, 32, tmp);
				*P0(dp) = tmp;
				GETBITS(P1(sp), sboff, 32, tmp);
				*P1(dp) = tmp;
				GETBITS(P2(sp), sboff, 32, tmp);
				*P2(dp) = tmp;
				GETBITS(P3(sp), sboff, 32, tmp);
				*P3(dp) = tmp;
			}

			if (lmask != 0) {
				if (sbover)
					sp -= BYTESDONE;
				dp -= BYTESDONE;
				GETBITS(P0(sp), sb, lnum, tmp);
				PUTBITS(tmp, db, lnum, P0(dp));
				GETBITS(P1(sp), sb, lnum, tmp);
				PUTBITS(tmp, db, lnum, P1(dp));
				GETBITS(P2(sp), sb, lnum, tmp);
				PUTBITS(tmp, db, lnum, P2(dp));
				GETBITS(P3(sp), sb, lnum, tmp);
				PUTBITS(tmp, db, lnum, P3(dp));
			}

			sp = (sq += scanspan);
			dp = (dq += scanspan);
			height--;
		}
	}
}

/*
 * Map a character.
 */
static int
om_mapchar(void *cookie, int c, u_int *cp)
{
	struct rasops_info *ri = cookie;
	struct wsdisplay_font *wf = ri->ri_font;

	if (wf->encoding != WSDISPLAY_FONTENC_ISO) {
		c = wsfont_map_unichar(wf, c);

		if (c < 0)
			goto fail;
	}
	if (c < wf->firstchar || c >= (wf->firstchar + wf->numchars))
		goto fail;

	*cp = c;
	return 5;

 fail:
	*cp = ' ';
	return 0;
}

/*
 * Position|{enable|disable} the cursor at the specified location.
 */
static void
om1_cursor(void *cookie, int on, int row, int col)
{
	struct rasops_info *ri = cookie;
	uint8_t *p;
	int scanspan, startx, height, width, align, y;
	uint32_t lmask, rmask;

	if (!on) {
		/* make sure it's on */
		if ((ri->ri_flg & RI_CURSOR) == 0)
			return;

		row = ri->ri_crow;
		col = ri->ri_ccol;
	} else {
		/* unpaint the old copy. */
		ri->ri_crow = row;
		ri->ri_ccol = col;
	}

	scanspan = ri->ri_stride;
	y = ri->ri_font->fontheight * row;
	startx = ri->ri_font->fontwidth * col;
	height = ri->ri_font->fontheight;

	p = (uint8_t *)ri->ri_bits + y * scanspan + ((startx / 32) * 4);
	align = startx & ALIGNMASK;
	width = ri->ri_font->fontwidth + align;
	lmask = ALL1BITS >> align;
	rmask = ALL1BITS << (-width & ALIGNMASK);
	if (width <= BLITWIDTH) {
		lmask &= rmask;
		/* set lmask as ROP mask value, with INV2 mode */
		((volatile uint32_t *)OMFB_ROPFUNC)[ROP_INV2] = lmask;

		while (height > 0) {
			*P0(p) = ALL1BITS;
			p += scanspan;
			height--;
		}
		/* reset mask value */
		((volatile uint32_t *)OMFB_ROPFUNC)[ROP_THROUGH] = ALL1BITS;
	} else {
		uint8_t *q = p;

		while (height > 0) {
			/* set lmask as ROP mask value, with INV2 mode */
			((volatile uint32_t *)OMFB_ROPFUNC)[ROP_INV2] = lmask;
			*W(p) = ALL1BITS;

			p += BYTESDONE;

			/* set lmask as ROP mask value, with INV2 mode */
			((volatile uint32_t *)OMFB_ROPFUNC)[ROP_INV2] = rmask;
			*W(p) = ALL1BITS;

			p = (q += scanspan);
			height--;
		}
		/* reset mask value */
		((volatile uint32_t *)OMFB_ROPFUNC)[ROP_THROUGH] = ALL1BITS;
	}
	ri->ri_flg ^= RI_CURSOR;
}

static void
om4_cursor(void *cookie, int on, int row, int col)
{
#if 0
	struct rasops_info *ri = cookie;
	uint8_t *p;
	int scanspan, startx, height, width, align, y;
	uint32_t lmask, rmask;

	if (!on) {
		/* make sure it's on */
		if ((ri->ri_flg & RI_CURSOR) == 0)
			return;

		row = ri->ri_crow;
		col = ri->ri_ccol;
	} else {
		/* unpaint the old copy. */
		ri->ri_crow = row;
		ri->ri_ccol = col;
	}

	scanspan = ri->ri_stride;
	y = ri->ri_font->fontheight * row;
	startx = ri->ri_font->fontwidth * col;
	height = ri->ri_font->fontheight;

	p = (uint8_t *)ri->ri_bits + y * scanspan + ((startx / 32) * 4);
	align = startx & ALIGNMASK;
	width = ri->ri_font->fontwidth + align;
	lmask = ALL1BITS >> align;
	rmask = ALL1BITS << (-width & ALIGNMASK);

	/* select all planes for later ROP function target */
	*(volatile uint32_t *)OMFB_PLANEMASK = 0xff;

	if (width <= BLITWIDTH) {
		lmask &= rmask;
		/* set lmask as ROP mask value, with INV2 mode */
		((volatile uint32_t *)OMFB_ROPFUNC)[ROP_INV2] = lmask;

		while (height > 0) {
			*W(p) = ALL1BITS;
			p += scanspan;
			height--;
		}
		/* reset mask value */
		((volatile uint32_t *)OMFB_ROPFUNC)[ROP_THROUGH] = ALL1BITS;
	} else {
		uint8_t *q = p;

		while (height > 0) {
			/* set lmask as ROP mask value, with INV2 mode */
			((volatile uint32_t *)OMFB_ROPFUNC)[ROP_INV2] = lmask;
			*W(p) = ALL1BITS;

			p += BYTESDONE;

			/* set rmask as ROP mask value, with INV2 mode */
			((volatile uint32_t *)OMFB_ROPFUNC)[ROP_INV2] = rmask;
			*W(p) = ALL1BITS;

			p = (q += scanspan);
			height--;
		}
		/* reset mask value */
		((volatile uint32_t *)OMFB_ROPFUNC)[ROP_THROUGH] = ALL1BITS;
	}
	/* select plane #0 only; XXX need this ? */
	*(volatile uint32_t *)OMFB_PLANEMASK = 0x01;

	ri->ri_flg ^= RI_CURSOR;
#else

	struct rasops_info *ri = cookie;
	int startx;
	int width;
	int height;
	int sh, sl;
	int y;
	int scanspan;
	uint8_t *p;

	if (!on) {
		/* make sure it's on */
		if ((ri->ri_flg & RI_CURSOR) == 0)
			return;

		row = ri->ri_crow;
		col = ri->ri_ccol;
	} else {
		/* unpaint the old copy. */
		ri->ri_crow = row;
		ri->ri_ccol = col;
	}

	scanspan = ri->ri_stride;
	y = ri->ri_font->fontheight * row;
	startx = ri->ri_font->fontwidth * col;
	width = ri->ri_font->fontwidth;
	height = ri->ri_font->fontheight;
	sh = startx >> 5;
	sl = startx & 0x1f;
	p = (uint8_t *)ri->ri_bits + y * scanspan + sh * 4;

	/* ROP_INV2: result = ~VRAM (ignore data from MPU) */
	om_fill(omfb_planemask, ROP_INV2,
		p, sl, scanspan,
		0, width, height);

	ri->ri_flg ^= RI_CURSOR;

	/* reset mask value */
	/* 先に ROP を設定するプレーンマスクを全プレーンにセット */
	omfb_setROP_curplane(ROP_THROUGH, ALL1BITS);
#endif
}

/*
 * Allocate attribute. We just pack these into an integer.
 */
static int
om1_allocattr(void *id, int fg, int bg, int flags, long *attrp)
{

	if ((flags & (WSATTR_HILIT | WSATTR_BLINK |
	    WSATTR_UNDERLINE | WSATTR_WSCOLORS)) != 0)
		return EINVAL;
	if ((flags & WSATTR_REVERSE) != 0)
		*attrp = 1;
	else
		*attrp = 0;
	return 0;
}

static int
om4_allocattr(void *id, int fg, int bg, int flags, long *attrp)
{

	if ((flags & (WSATTR_BLINK | WSATTR_UNDERLINE)) != 0)
		return EINVAL;
	if ((flags & WSATTR_WSCOLORS) == 0) {
		fg = WSCOL_WHITE;
		bg = WSCOL_BLACK;
	}

	if ((flags & WSATTR_REVERSE) != 0) {
		int swap;
		swap = fg;
		fg = bg;
		bg = swap;
	}

	if ((flags & WSATTR_HILIT) != 0)
		fg += 8;

	*attrp = (fg << 24) | (bg << 16);

#if defined(VT100_SIXEL)
	if (flags & WSATTR_SIXEL) {
		*attrp |= (flags >> 16) & 3;
	}
#endif

	return 0;
}

static void
om4_unpack_attr(long attr, int *fg, int *bg, int *underline)
{

	*fg = ((u_int)attr >> 24) & 0xf;
	*bg = ((u_int)attr >> 16) & 0xf;
}

/*
 * Init subset of rasops(9) for omrasops.
 */
int
omrasops1_init(struct rasops_info *ri, int wantrows, int wantcols)
{

	omrasops_init(ri, wantrows, wantcols);

	/* fill our own emulops */
	ri->ri_ops.cursor    = om1_cursor;
	ri->ri_ops.mapchar   = om_mapchar;
	ri->ri_ops.putchar   = om1_putchar;
	ri->ri_ops.copycols  = om1_copycols;
	ri->ri_ops.erasecols = om1_erasecols;
	ri->ri_ops.copyrows  = om1_copyrows;
	ri->ri_ops.eraserows = om1_eraserows;
	ri->ri_ops.allocattr = om1_allocattr;
	ri->ri_caps = WSSCREEN_REVERSE;

	ri->ri_flg |= RI_CFGDONE;

	return 0;
}

int
omrasops4_init(struct rasops_info *ri, int wantrows, int wantcols)
{

	omrasops_init(ri, wantrows, wantcols);

	/* fill our own emulops */
	ri->ri_ops.cursor    = om4_cursor;
	ri->ri_ops.mapchar   = om_mapchar;
	ri->ri_ops.putchar   = om4_putchar;
	ri->ri_ops.copycols  = om4_copycols;
	ri->ri_ops.erasecols = om4_erasecols;
	ri->ri_ops.copyrows  = om4_copyrows;
	ri->ri_ops.eraserows = om4_eraserows;
	ri->ri_ops.allocattr = om4_allocattr;
	ri->ri_caps = WSSCREEN_HILIT | WSSCREEN_WSCOLORS | WSSCREEN_REVERSE;

	ri->ri_flg |= RI_CFGDONE;

	return 0;
}

static int
omrasops_init(struct rasops_info *ri, int wantrows, int wantcols)
{
	int wsfcookie, bpp;

	if (wantrows == 0)
		wantrows = 34;
	if (wantrows < 10)
		wantrows = 10;
	if (wantcols == 0)
		wantcols = 80;
	if (wantcols < 20)
		wantcols = 20;

	/* Use default font */
	wsfont_init();
	wsfcookie = wsfont_find(NULL, 0, 0, 0, WSDISPLAY_FONTORDER_L2R,
	    WSDISPLAY_FONTORDER_L2R, WSFONT_FIND_BITMAP);
	if (wsfcookie < 0)
		panic("%s: no font available", __func__);
	if (wsfont_lock(wsfcookie, &ri->ri_font))
		panic("%s: unable to lock font", __func__);
	ri->ri_wsfcookie = wsfcookie;

	KASSERT(ri->ri_font->fontwidth > 4 && ri->ri_font->fontwidth <= 32);

	/* all planes are independently addressed */
	bpp = 1;

	/* Now constrain what they get */
	ri->ri_emuwidth = ri->ri_font->fontwidth * wantcols;
	ri->ri_emuheight = ri->ri_font->fontheight * wantrows;
	if (ri->ri_emuwidth > ri->ri_width)
		ri->ri_emuwidth = ri->ri_width;
	if (ri->ri_emuheight > ri->ri_height)
		ri->ri_emuheight = ri->ri_height;

	/* Reduce width until aligned on a 32-bit boundary */
	while ((ri->ri_emuwidth * bpp & 31) != 0)
		ri->ri_emuwidth--;

	ri->ri_cols = ri->ri_emuwidth / ri->ri_font->fontwidth;
	ri->ri_rows = ri->ri_emuheight / ri->ri_font->fontheight;
	ri->ri_emustride = ri->ri_emuwidth * bpp >> 3;
	ri->ri_ccol = 0;
	ri->ri_crow = 0;
	ri->ri_pelbytes = bpp >> 3;

	ri->ri_xscale = (ri->ri_font->fontwidth * bpp) >> 3;
	ri->ri_yscale = ri->ri_font->fontheight * ri->ri_stride;
	ri->ri_fontscale = ri->ri_font->fontheight * ri->ri_font->stride;

	/* Clear the entire display */
	if ((ri->ri_flg & RI_CLEAR) != 0)
		memset(ri->ri_bits, 0, ri->ri_stride * ri->ri_height);

	/* Now centre our window if needs be */
	ri->ri_origbits = ri->ri_bits;

	if ((ri->ri_flg & RI_CENTER) != 0) {
		ri->ri_bits += (((ri->ri_width * bpp >> 3) -
		    ri->ri_emustride) >> 1) & ~3;
		ri->ri_bits += ((ri->ri_height - ri->ri_emuheight) >> 1) *
		    ri->ri_stride;
		ri->ri_yorigin = (int)(ri->ri_bits - ri->ri_origbits)
		   / ri->ri_stride;
		ri->ri_xorigin = (((int)(ri->ri_bits - ri->ri_origbits)
		   % ri->ri_stride) * 8 / bpp);
	} else
		ri->ri_xorigin = ri->ri_yorigin = 0;

	return 0;
}

#if defined(VT100_SIXEL)
/*
 * rendering SIXEL graphics
 * y: (row-relative-y [pixel]) << 16 | row
 * x: (col-relative-x [pixel]) << 16 | col
 * uc: sixel char
 * attr: (attr:fg) = color
 *       (attr:16:16) = sixel repeat count
 */
static void
omfb_sixel(struct rasops_info *ri, int yrow, int xcol, u_int uc, long attr)
{
#if 0
	const uint32_t table[64] = {
		0x000000,0x000001,0x000010,0x000011,0x000100,0x000101,0x000110,0x000111,
		0x001000,0x001001,0x001010,0x001011,0x001100,0x001101,0x001110,0x001111,
		0x010000,0x010001,0x010010,0x010011,0x010100,0x010101,0x010110,0x010111,
		0x011000,0x011001,0x011010,0x011011,0x011100,0x011101,0x011110,0x011111,
		0x100000,0x100001,0x100010,0x100011,0x100100,0x100101,0x100110,0x100111,
		0x101000,0x101001,0x101010,0x101011,0x101100,0x101101,0x101110,0x101111,
		0x110000,0x110001,0x110010,0x110011,0x110100,0x110101,0x110110,0x110111,
		0x111000,0x111001,0x111010,0x111011,0x111100,0x111101,0x111110,0x111111,
	};
#endif
	uint32_t ptn;
	int xh, xl;
	int fg, bg;
	int dw;
	int i;
	uint8_t *dst;
	uint32_t mask;
	int16_t ormode;
	int16_t width;
	int col;
	int x;
	int row;
	int y;

	width = (uc >> 16) & 2047;
	uc &= 0xffff;
	if ('?' <= uc && uc <= '~') {
		ptn = uc - '?';
	} else {
		return;
	}

	col = xcol & 0xffff;
	x = (xcol >> 16) & 0xffff;
	x += col * ri->ri_font->fontwidth;
	row = yrow & 0xffff;
	y = (yrow >> 16) & 0xffff;

	rowattr[row].ismulti = 1;
	if (y + 5 > ri->ri_font->fontheight) {
		// 行をまたぐ書き込み
		// rowattr は余裕を見て確保されている
		rowattr[row + 1].ismulti = 1;
	}

	y += row * ri->ri_font->fontheight;

	xh = x >> 5;
	xl = x & 0x1f;

	om4_unpack_attr(attr, &fg, &bg, NULL);

	dst = (uint8_t *)ri->ri_bits + y * ri->ri_stride + xh * 4;
	ormode = attr & 0x2;

	/*
	ormode
		fg D  result
		0  0  M
		0  1  M   planemasked
		1  0  M
		1  1  1   D+M = OR1
	not ormode (overwrite mode)
		fg D  result
		0  0  M
		0  1  0   ~D*M = AND2
		1  0  M
		1  1  1   D+M = OR1
	*/

	if (width == 1) {
		// ほとんどのケースで width == 1 なのでファストパスする
		// 左からのビット位置に変換
		xl = 31 - xl;
		if (ormode) {
			omfb_setplanemask(fg);
			omfb_setROP_curplane(ROP_OR1, 1 << xl);
		} else {
			omfb_setplanemask(omfb_planemask);
			omfb_setROP_curplane(ROP_AND2, 1 << xl);
			omfb_setplanemask(fg);
			omfb_setROP_curplane(ROP_OR1, 1 << xl);
			omfb_setplanemask(omfb_planemask);
		}

		// 左ローテート命令にコンパイルされてほしい
		// rol.l	%[xl],%[ptn]
		ptn = (ptn << xl) | (ptn >> (32 - xl));

		for (i = 0; i < 6; i++) {
			// ROP masked xl bit
			*W(dst) = ptn;
			dst += ri->ri_stride;
			// 右ローテート命令にコンパイルされてほしい
			// ror.l	#1,%[ptn]
			ptn = (ptn >> 1) | (ptn << 31);
		}
		return;
	}

	/* fill のアルゴリズム */

	uint32_t v[6];
	v[0] = (ptn & 0x01) ? ALL1BITS : ALL0BITS;
	v[1] = (ptn & 0x02) ? ALL1BITS : ALL0BITS;
	v[2] = (ptn & 0x04) ? ALL1BITS : ALL0BITS;
	v[3] = (ptn & 0x08) ? ALL1BITS : ALL0BITS;
	v[4] = (ptn & 0x10) ? ALL1BITS : ALL0BITS;
	v[5] = (ptn & 0x20) ? ALL1BITS : ALL0BITS;


	mask = ALL1BITS >> xl;
	dw = 32 - xl;

	do {
		width -= dw;
		if (width < 0) {
			width = -width;
			MASK_CLEAR_RIGHT(mask, width);

			width = 0;
		}

		if (ormode) {
			omfb_setplanemask(fg);
			omfb_setROP_curplane(ROP_OR1, mask);
		} else {
			omfb_setplanemask(omfb_planemask);
			omfb_setROP_curplane(ROP_AND2, mask);
			omfb_setplanemask(fg);
			omfb_setROP_curplane(ROP_OR1, mask);
			omfb_setplanemask(omfb_planemask);
		}

		{
			uint8_t *d = dst;
			for (i = 0; i < 6; i++) {
				// ROP masked
				*W(d) = v[i];
				d += ri->ri_stride;
			}
		}
		dst += 4;
		mask = ALL1BITS;
		dw = 32;
	} while (width > 0);
}
#endif
