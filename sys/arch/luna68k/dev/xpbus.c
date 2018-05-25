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
static int xpbus_print(void *, const char *);

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
		config_found(self, __UNCONST(xa), xpbus_print);
	}
}

static int
xpbus_print(void *aux, const char *pnp)
{
/*
	struct xpbus_attach_args *xa = aux;

	if (pnp)
		aprint_normal("%s at %s", xa->xa_name, pnp);
*/

	return QUIET;
}
