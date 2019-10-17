/* $NetBSD: omrasopsvar.h,v 1.5 2019/09/22 05:49:16 rin Exp $ */
/*
 * Copyright (c) 2013 Kenji Aoyama
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
 * Base addresses of LUNA's frame buffer
 * XXX: We consider only 1bpp and 4bpp for now
 */

#include <machine/board.h>

/*
BMSEL は共通ビットマッププレーンへの書き込みがどのプレーンに反映されるか、
および共通 ROP への書き込みがどのプレーンの ROP に反映されるかの
両方に影響する。

共通 ROP へのセットは、現在プレーンマスクで選択されている
プレーンの ROP をひとつづつセットしていくのと同じ効果がある。
共通プレーンへ書き込んだときの ROP という意味ではない。
*/

#define OMFB_PLANEMASK	BMAP_BMSEL	/* BMSEL register */

// XXX: maybe move to lunafb.c
// these are no mean for users, also omrasops.c
// XXX もともと 8 バイト足してあったが、これだと
// VRAM の狙ったアドレスにアクセスできないので、やめてみる。
//#define OMFB_FB_WADDR	(BMAP_BMP + 8)	/* common plane */
//#define OMFB_FB_RADDR	(BMAP_BMAP0 + 8)/* plane #0 */
#define OMFB_FB_WADDR	(BMAP_BMP)		/* common plane */
#define OMFB_FB_RADDR	(BMAP_BMAP0)	/* plane #0 */

#define OMFB_ROPFUNC	BMAP_FN		/* common ROP function */
#define OMFB_PLANEOFS	(0x40000)	/* plane offset */

#define OMFB_RASTERBYTES	(2048/8) /* bytes in VRAM raster */


#define OMFB_MAX_PLANECOUNT	(8)

/* 個別のプレーンや ROP の処理を行うため、定義をやり直す */

#define OMFB_PLANE_C	BMAP_BMP		/* common plane */
#define OMFB_PLANE_0	BMAP_BMAP0

#define OMFB_ROP_C		BMAP_FN			/* common ROP */
#define OMFB_ROP_0		BMAP_FN0

#define OMFB_STRIDE		(2048/8)		/* stride [byte] */


/* 差し替え予定 */
#define omfb_planemask	hwplanemask
#define omfb_planecount hwplanecount
extern int hwplanemask;
extern int hwplanecount;

/*
 * Helper macros
 */
#define W(addr)  ((uint32_t *)(addr))

/* obsoleted */
#define P0(addr) ((uint32_t *)((uint8_t *)(addr) + OMFB_PLANEOFS * 1))
#define P1(addr) ((uint32_t *)((uint8_t *)(addr) + OMFB_PLANEOFS * 2))
#define P2(addr) ((uint32_t *)((uint8_t *)(addr) + OMFB_PLANEOFS * 3))
#define P3(addr) ((uint32_t *)((uint8_t *)(addr) + OMFB_PLANEOFS * 4))

/*
 * ROP function
 *
 * LUNA's frame buffer uses Hitachi HM53462 video RAM, which has raster
 * (logic) operation, or ROP, function.  To use ROP function on LUNA, write
 * a 32bit `mask' value to the specified address corresponding to each ROP
 * logic.
 *
 * D: the data writing to the video RAM
 * M: the data already stored on the video RAM
 */

/* operation		index	the video RAM contents will be */
#define ROP_ZERO	 0	/* all 0	*/
#define ROP_AND1	 1	/* D & M	*/ 
#define ROP_AND2	 2	/* ~D & M	*/
/* Not used on LUNA	 3			*/
#define ROP_AND3	 4	/* D & ~M	*/
#define ROP_THROUGH	 5	/* D		*/
#define ROP_EOR		 6	/* (~D & M) | (D & ~M)	*/
#define ROP_OR1		 7	/* D | M	*/
#define ROP_NOR		 8	/* ~D | ~M	*/
#define ROP_ENOR	 9	/* (D & M) | (~D & ~M)	*/
#define ROP_INV1	10	/* ~D		*/
#define ROP_OR2		11	/* ~D | M	*/
#define ROP_INV2	12	/* ~M		*/
#define ROP_OR3		13	/* D | ~M	*/
#define ROP_NAND	14	/* ~D | ~M	*/
#define ROP_ONE		15	/* all 1	*/

/*
 * internal attributes. see allocattr
 */
#define OMFB_ATTR_SIXEL			(1U << 31)
#define OMFB_ATTR_MULTICOLOR		(1U << 30)
#define OMFB_ATTR_UNDERLINE		(1U << 17)
#define OMFB_ATTR_BOLD			(1U << 16)

/*
 */

int omrasops1_init(struct rasops_info *, int, int);
int omrasops4_init(struct rasops_info *, int, int);

/*
 * planemask and ROP inline functions
 */

/* set planemask for common plane and common ROP */
static inline void
omfb_setplanemask(int planemask)
{
	*(volatile uint32_t *)OMFB_PLANEMASK = planemask;
}

/* set ROP and ROP's mask for individual plane */
static inline void
omfb_setROP(int plane, int rop, uint32_t mask)
{
	((volatile uint32_t *)(OMFB_ROP_0 + OMFB_PLANEOFS * plane))[rop] = mask;
}

/* get ROP address */
static inline uint32_t *
omfb_ROPaddr(int plane, int rop)
{
	return (uint32_t *)(OMFB_ROP_0 + OMFB_PLANEOFS * plane + rop * 4);
}

/* set ROP and ROP's mask for current setplanemask-ed plane(s) */
static inline void
omfb_setROP_curplane(int rop, uint32_t mask)
{
	((volatile uint32_t *)(OMFB_ROP_C))[rop] = mask;
}

/* reset planemask and ROP */
static inline void
omfb_resetplanemask_and_ROP(void)
{
	omfb_setplanemask(omfb_planemask);
	omfb_setROP_curplane(ROP_THROUGH, ~0U);
}
