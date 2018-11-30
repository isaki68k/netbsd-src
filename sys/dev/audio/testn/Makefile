.include "../Makefile.test"

all:	ntest.${MACHINE} ntest.x68k

ntest.${MACHINE}:	ntest.cpp
	${CXX.${MACHINE}} ${CPPFLAGS} -o $@ $> $(LIBS)

ntest.x68k:	ntest.cpp
	${CXX.x68k} $(CPPFLAGS) -o $@ $> $(LIBS)

clean:
	rm -f ntest.${MACHINE} ntest.x68k *.o *.core
