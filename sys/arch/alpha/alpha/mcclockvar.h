/* $NetBSD: mcclockvar.h,v 1.7 2024/03/06 07:34:11 thorpej Exp $ */

/*
 * Copyright (c) 1996 Carnegie-Mellon University.
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

#ifndef _ALPHA_ALPHA_MCCLOCKVAR_H_
#define	_ALPHA_ALPHA_MCCLOCKVAR_H_

struct mcclock_softc {
	struct mc146818_softc	sc_mc146818;

	/* Accessible only on the primary CPU. */
	bool			sc_primary_only;
};

void	mcclock_attach(struct mcclock_softc *);

#endif /* _ALPHA_ALPHA_MCCLOCKVAR_H_ */
