CPPFLAGS+=	-g

all:	ntest.$(MACHINE)

ntest.$(MACHINE):	ntest.o
	$(CC) -g -o $@ $>

clean:
	rm -rf ntest.o
