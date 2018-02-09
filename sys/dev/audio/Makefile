CPPFLAGS+=	-g
CPPFLAGS+=	-Wformat
LIBS+=	-lutil

all:	ncmd.${MACHINE} ntest.$(MACHINE) ncmd.x68k ntest.x68k

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

clean:
	rm -rf ncmd.o ntest.o
