CPPFLAGS+=	-g
CPPFLAGS+=	-Wformat
LIBS+=	-lutil

CPPFLAGS.ltest=	-I. -Inetbsd

all:	ncmd.${MACHINE} ntest.$(MACHINE) ncmd.x68k ntest.x68k ltest

ncmd.$(MACHINE):	ncmd.o
	$(CC) -g -o $@ $>

ntest.$(MACHINE):	ntest.o
	$(CC) -g -o $@ $> $(LIBS)

ncmd.x68k: ncmd.cpp
	/var/obj/7-1/x68k/tools/bin/m68k--netbsdelf-g++ \
		--sysroot=/var/obj/7-1/x68k/dst \
		$(CPPFLAGS) \
		-o $@ $> $(LIBS)

ntest.x68k:	ntest.cpp
	/var/obj/7-1/x68k/tools/bin/m68k--netbsdelf-g++ \
		--sysroot=/var/obj/7-1/x68k/dst \
		$(CPPFLAGS) \
		-o $@ $> $(LIBS)

ltest:	ltest.c
	$(CC) ${CPPFLAGS} ${CPPFLAGS.ltest} -std=c99 -o $@ $> $(LIBS)

clean:
	rm -rf ncmd.o ntest.o ltest
