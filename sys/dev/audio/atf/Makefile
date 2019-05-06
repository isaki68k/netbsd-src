#	$NetBSD$
#

.include <bsd.own.mk>

# XXX
CPPFLAGS+=	-I/rina/NetBSD/current/src/tests

TESTS_C=	t_audio

LDADD.t_audio+=	-lrump -lrumpdev_audio -lrumpdev_pad -lrumpdev -lrumpvfs

MKMAN=no
#BINDIR=		${TESTDIR}

.include <bsd.test.mk>
