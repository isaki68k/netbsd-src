#	$NetBSD: Makefile,v 1.3 2019/02/17 04:05:43 rin Exp $

.include "../Makefile.inc"

.PATH:	${S}/dev/audio

KMOD=	audio
IOCONF=	audio.ioconf
SRCS=	audio.c \
	alaw.c \
	linear.c \
	mulaw.c

CPPFLAGS+=	-DNAUDIO=1

WARNS=	3

.include <bsd.kmodule.mk>
