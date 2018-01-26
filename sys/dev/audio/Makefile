CPPFLAGS+=	-g

all:	ncmd.${MACHINE} ntest.$(MACHINE)

ncmd.$(MACHINE):	ncmd.o
	$(CC) -g -o $@ $>

ntest.$(MACHINE):	ntest.o
	$(CC) -g -o $@ $>

clean:
	rm -rf ncmd.o ntest.o
