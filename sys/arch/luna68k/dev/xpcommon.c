/* $NetBSD$ */

/*-
 * Copyright (c) 2016 Izumi Tsutsui.  All rights reserved.
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

/* TODO: ao-kenji 's copyright */
/* TODO: my copyright */

/*
 * LUNA's Hitachi HD647180 "XP" I/O processor common routines
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>

#include "xpcommon.h"

/*
 * PIO 0 port C is connected to XP's reset line
 *
 * XXX: PIO port functions should be shared with machdep.c for DIP SWs
 */
#define PIO_ADDR	0x49000000
#define PORT_A		0
#define PORT_B		1
#define PORT_C		2
#define CTRL		3
#define OBIO_PIO0A	(PIO_ADDR + PORT_A)
#define OBIO_PIO0B	(PIO_ADDR + PORT_B)

/* PIO0 Port C bit definition */
#define XP_INT1_REQ	0	/* INTR B */
	/* unused */		/* IBF B */
#define XP_INT1_ENA	2	/* INTE B */
#define XP_INT5_REQ	3	/* INTR A */
#define XP_INT5_ENA	4	/* INTE A */
	/* unused */		/* IBF A */
#define PARITY		6	/* PC6 output to enable parity error */
#define XP_RESET	7	/* PC7 output to reset HD647180 XP */

/* Port control for PC6 and PC7 */
#define ON		1
#define OFF		0

uint8_t
put_pio0c(uint8_t bit, uint8_t set)
{
	volatile uint8_t * const pio0 = (uint8_t *)PIO_ADDR;

	pio0[CTRL] = (bit << 1) | (set & 0x01);

	return pio0[PORT_C];
}

void
xp_cpu_reset_hold(void)
{
	put_pio0c(XP_RESET, ON);
}

void
xp_cpu_reset_release(void)
{
	put_pio0c(XP_RESET, OFF);
}

void
xp_cpu_reset(void)
{
	xp_cpu_reset_hold();
	delay(100);
	xp_cpu_reset_release();
}

void
xp_intr1_enable(void)
{
	put_pio0c(XP_INT1_ENA, ON);
}

void
xp_intr1_disable(void)
{
	put_pio0c(XP_INT1_ENA, OFF);
}

void
xp_intr1_acknowledge(void)
{
	/* reset the interrupt request: read PIO0 port A */
	// XXX: たぶん
	*(volatile uint8_t *)OBIO_PIO0A;
	// XXX: たちまち、勘で。
	*(volatile uint8_t *)OBIO_PIO0B;
}

void
xp_intr5_enable(void)
{
	put_pio0c(XP_INT5_ENA, ON);
}

void
xp_intr5_disable(void)
{
	put_pio0c(XP_INT5_ENA, OFF);
}

void
xp_intr5_acknowledge(void)
{
	/* reset the interrupt request: read PIO0 port A */
	*(volatile uint8_t *)OBIO_PIO0A;
}

uint8_t *
xp_shmptr(int offset)
{
	return (uint8_t *)XP_SHM_BASE + offset;
}

int
xp_readmem8(int offset)
{
	return *((volatile uint8_t *)xp_shmptr(offset));
}

int
xp_readmem16le(int offset)
{
	return le16toh(*(volatile uint16_t *)xp_shmptr(offset));
}

void
xp_writemem8(int offset, int v)
{
	*(volatile uint8_t *)(xp_shmptr(offset)) = v;
}

void
xp_writemem16le(int offset, int v)
{
	*((volatile uint16_t *)xp_shmptr(offset)) = htole16((uint16_t)v);
}

