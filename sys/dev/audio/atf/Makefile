#	$NetBSD$
#

.include <bsd.own.mk>

# XXX t_audio.sh is generated from Makefile.t_audio
TESTS_SH=	t_audio

PROGS=		h_audio
CPPFLAGS+=	-g
CPPFLAGS+=	-Wno-missing-prototypes	# it's too severe to testcode
LDADD+=	-lrump -lrumpdev_audio -lrumpdev_pad -lrumpdev -lrumpvfs

WARNS=4
MKMAN=no

.include <bsd.test.mk>
