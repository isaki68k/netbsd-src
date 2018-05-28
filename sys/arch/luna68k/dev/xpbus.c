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

/*
 * LUNA's Hitachi HD647180 "XP" I/O processor
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/device.h>

#include <machine/autoconf.h>

#include <luna68k/dev/xpbusvar.h>
#include <luna68k/dev/xpcommon.h>

struct xpbus_softc {
	device_t	sc_dev;
};

static const struct xpbus_attach_args xpdevs[] = {
	{ "xp" },
	{ "psgpam" },
};

static int xpbus_match(device_t, cfdata_t, void *);
static void xpbus_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(xpbus, sizeof(struct xpbus_softc),
    xpbus_match, xpbus_attach, NULL, NULL);

static bool xpbus_matched;

static int
xpbus_match(device_t parent, cfdata_t cf, void *aux)
{
	struct mainbus_attach_args *ma = aux;

	/* only one XP processor */
	if (xpbus_matched)
		return 0;

	if (ma->ma_addr != XP_SHM_BASE)
		return 0;

	xpbus_matched = true;
	return 1;
}

static void
xpbus_attach(device_t parent, device_t self, void *aux)
{
	struct xpbus_softc *sc = device_private(self);
	const struct xpbus_attach_args *xa;
	int i;

	sc->sc_dev = self;
	aprint_normal("\n");

	for (i = 0; i < __arraycount(xpdevs); i++) {
		xa = &xpdevs[i];
		config_found(self, __UNCONST(xa), NULL);
	}
}
