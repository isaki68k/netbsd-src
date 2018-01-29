CPPFLAGS+=	-g
CPPFLAGS+=	-Wformat

all:	ncmd.${MACHINE} ntest.$(MACHINE) ntest.x68k

ncmd.$(MACHINE):	ncmd.o
	$(CC) -g -o $@ $>

ntest.$(MACHINE):	ntest.o
	$(CC) -g -o $@ $>

ntest.x68k:	ntest.cpp
	/var/obj/7-1/x68k/tools/bin/m68k--netbsdelf-g++ \
		--sysroot=/var/obj/7-1/x68k/dst \
		$(CPPFLAGS) \
		-o $@ $>

clean:
	rm -rf ncmd.o ntest.o
