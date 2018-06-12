/*	$NetBSD$	*/

/*
 * Copyright (c) 2018 Tetsuya Isaki. All rights reserved.
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
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _XPBUSVAR_H_
#define _XPBUSVAR_H_

#define XP_SHM_BASE	0x71000000
#define XP_SHM_SIZE	0x00010000	/* 64KB for XP; rest 64KB for lance */
#define XP_TAS_ADDR	0x61000000

struct xpbus_attach_args {
	const char *xa_name;
};

int  xp_acquire(void);
void xp_release(void);

uint8_t put_pio0c(uint8_t, uint8_t);

void xp_cpu_reset_hold(void);
void xp_cpu_reset_release(void);
void xp_cpu_reset(void);

void xp_intr1_enable(void);
void xp_intr1_disable(void);
void xp_intr1_acknowledge(void);

void xp_intr5_enable(void);
void xp_intr5_disable(void);
void xp_intr5_acknowledge(void);

uint8_t *xp_shmptr(int);
int  xp_readmem8(int);
int  xp_readmem16le(int);
void xp_writemem8(int, int);
void xp_writemem16le(int, int);

#endif /* !_XPBUSVAR_H_ */