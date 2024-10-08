/* $NetBSD: dec_3000_500.c,v 1.48 2024/03/31 19:06:30 thorpej Exp $ */

/*
 * Copyright (c) 1994, 1995, 1996 Carnegie-Mellon University.
 * All rights reserved.
 *
 * Author: Chris G. Demetriou
 *
 * Permission to use, copy, modify and distribute this software and
 * its documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND
 * FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie the
 * rights to redistribute these changes.
 */
/*
 * Additional Copyright (c) 1997 by Matthew Jacob for NASA/Ames Research Center
 */

#include <sys/cdefs.h>			/* RCS ID & Copyright macro defns */

__KERNEL_RCSID(0, "$NetBSD: dec_3000_500.c,v 1.48 2024/03/31 19:06:30 thorpej Exp $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/termios.h>
#include <sys/conf.h>
#include <dev/cons.h>

#include <machine/rpb.h>
#include <machine/autoconf.h>
#include <machine/cpuconf.h>

#include <dev/tc/tcvar.h>
#include <dev/tc/tcdsvar.h>
#include <alpha/tc/tc_3000_500.h>

#include <machine/z8530var.h>
#include <dev/tc/zs_ioasicvar.h>

#include <dev/scsipi/scsi_all.h>
#include <dev/scsipi/scsipi_all.h>
#include <dev/scsipi/scsiconf.h>

#include "wsdisplay.h"

void dec_3000_500_init(void);
static void dec_3000_500_cons_init(void);
static void dec_3000_500_device_register(device_t, void *);

static const char dec_3000_500_sp[] = "DEC 3000/400 (\"Sandpiper\")";
static const char dec_3000_500_sf[] = "DEC 3000/500 (\"Flamingo\")";

const struct alpha_variation_table dec_3000_500_variations[] = {
	{ SV_ST_SANDPIPER, dec_3000_500_sp },
	{ SV_ST_FLAMINGO, dec_3000_500_sf },
	{ SV_ST_HOTPINK, "DEC 3000/500X (\"Hot Pink\")" },
	{ SV_ST_FLAMINGOPLUS, "DEC 3000/800 (\"Flamingo+\")" },
	{ SV_ST_SANDPLUS, "DEC 3000/600 (\"Sandpiper+\")" },
	{ SV_ST_SANDPIPER45, "DEC 3000/700 (\"Sandpiper45\")" },
	{ SV_ST_FLAMINGO45, "DEC 3000/900 (\"Flamingo45\")" },
	{ 0, NULL },
};

void
dec_3000_500_init(void)
{
	uint64_t variation;

	platform.family = "DEC 3000/500 (\"Flamingo\")";

	if ((platform.model = alpha_dsr_sysname()) == NULL) {
		variation = hwrpb->rpb_variation & SV_ST_MASK;
		if (variation == SV_ST_ULTRA) {
			/* These are really the same. */
			variation = SV_ST_FLAMINGOPLUS;
		}
		if ((platform.model = alpha_variation_name(variation,
		    dec_3000_500_variations)) == NULL) {
			/*
			 * This is how things used to be done.
			 */
			if (variation == SV_ST_RESERVED) {
				if (hwrpb->rpb_variation & SV_GRAPHICS)
					platform.model = dec_3000_500_sf;
				else
					platform.model = dec_3000_500_sp;
			} else
				platform.model = alpha_unknown_sysname();
		}
	}

	platform.iobus = "tcasic";
	platform.cons_init = dec_3000_500_cons_init;
	platform.device_register = dec_3000_500_device_register;
}

static void
dec_3000_500_cons_init(void)
{
	struct ctb *ctb;

	ctb = (struct ctb *)(((char *)hwrpb) + hwrpb->rpb_ctb_off);

	switch (ctb->ctb_term_type) {
	case CTB_GRAPHICS:
#if NWSDISPLAY > 0
		/* display console ... */
		if (zs_ioasic_lk201_cnattach(0x1e0000000, 0x00180000, 0) == 0 &&
		    tc_3000_500_fb_cnattach(
		     CTB_TURBOSLOT_SLOT(ctb->ctb_turboslot)) == 0) {
			break;
		}
#endif
		printf("consinit: Unable to init console on keyboard and ");
		printf("TURBOchannel slot 0x%lx.\n", ctb->ctb_turboslot);
		printf("Using serial console.\n");
		/* FALLTHROUGH */

	case CTB_PRINTERPORT:
		/* serial console ... */
		/*
		 * XXX This could stand some cleanup...
		 */
		{
			/*
			 * Delay to allow PROM putchars to complete.
			 * FIFO depth * character time,
			 * character time = (1000000 / (defaultrate / 10))
			 */
			DELAY(160000000 / 9600);	/* XXX */

			/*
			 * Console is channel B of the second SCC.
			 * XXX Should use ctb_line_off to get the
			 * XXX line parameters--these are the defaults.
			 */
			zs_ioasic_cnattach(0x1e0000000, 0x00180000, 1);
			break;
		}

	default:
		printf("ctb->ctb_term_type = 0x%lx\n", ctb->ctb_term_type);
		printf("ctb->ctb_turboslot = 0x%lx\n", ctb->ctb_turboslot);
		panic("consinit: unknown console type %lu",
		    ctb->ctb_term_type);
		/* NOTREACHED */
	}
}

static void
dec_3000_500_device_register(device_t dev, void *aux)
{
	static int found, initted, scsiboot, netboot;
	static device_t scsidev;
	static device_t tcdsdev;
	struct bootdev_data *b = bootdev_data;
	device_t parent = device_parent(dev);

	if (b == NULL || found)
		return;

	if (!initted) {
		scsiboot = (strcmp(b->protocol, "SCSI") == 0);
		netboot = (strcmp(b->protocol, "BOOTP") == 0) ||
		    (strcmp(b->protocol, "MOP") == 0);
#if 0
		printf("scsiboot = %d, netboot = %d\n", scsiboot, netboot);
#endif
		initted = 1;
	}

	/*
	 * for scsi boot, we look for "tcds", make sure it has the
	 * right slot number, then find the "asc" on this tcds that
	 * as the right channel.  then we find the actual scsi
	 * device we came from.  note: no SCSI LUN support (yet).
	 */
	if (scsiboot && device_is_a(dev, "tcds")) {
		struct tc_attach_args *tcargs = aux;

		if (b->slot != tcargs->ta_slot)
			return;

		tcdsdev = dev;
#if 0
		printf("\ntcdsdev = %s\n", device_xname(dev));
#endif
	}
	if (scsiboot && tcdsdev &&
	    device_is_a(dev, "asc")) {
		struct tcdsdev_attach_args *ta = aux;

		if (parent != tcdsdev)
			return;

		if (ta->tcdsda_chip != b->channel)
			return;

		scsidev = dev;
#if 0
		printf("\nscsidev = %s\n", device_xname(dev));
#endif
	}

	if (scsiboot && scsidev &&
	    (device_is_a(dev, "sd") ||
	     device_is_a(dev, "st") ||
	     device_is_a(dev, "cd"))) {
		struct scsipibus_attach_args *sa = aux;

		if (device_parent(parent) != scsidev)
			return;

		if (b->unit / 100 != sa->sa_periph->periph_target)
			return;

		/* XXX LUN! */

		switch (b->boot_dev_type) {
		case 0:
			if (!device_is_a(dev, "sd") &&
			    !device_is_a(dev, "cd"))
				return;
			break;
		case 1:
			if (!device_is_a(dev, "st"))
				return;
			break;
		default:
			return;
		}

		/* we've found it! */
		booted_device = dev;
#if 0
		printf("\nbooted_device = %s\n", device_xname(booted_device));
#endif
		found = 1;
	}

	if (netboot) {
	        if (b->slot == 7 && device_is_a(dev, "le") &&
		    device_is_a(parent, "ioasic")) {
			/*
			 * no need to check ioasic_attach_args, since only
			 * one le on ioasic.
			 */

			booted_device = dev;
#if 0
			printf("\nbooted_device = %s\n", device_xname(booted_device));
#endif
			found = 1;
			return;
		}

		/*
		 * XXX GENERIC SUPPORT FOR TC NETWORK BOARDS
		 */
	}
}
