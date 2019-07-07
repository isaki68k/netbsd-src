.include "../Makefile.test"

all:	ntest.${MACHINE} ntest.x68k ntest.amd64-8

ntest.${MACHINE}:	ntest.cpp
	${CXX.${MACHINE}} -pthread ${CPPFLAGS} -o $@ $> $(LIBS)

ntest.x68k:	ntest.cpp
	${CXX.x68k} -pthread $(CPPFLAGS) -o $@ $> $(LIBS)

ntest.amd64-8:	ntest.cpp
	/var/obj/8/amd64/tools/bin/x86_64--netbsd-g++ --sysroot=/var/obj/8/amd64/dst -pthread ${CPPFLAGS} -o $@ $> ${LIBS}

ntest.alpha:	ntest.cpp
	${CXX.alpha} -pthread $(CPPFLAGS) -o $@ $> $(LIBS)

clean:
	rm -f ntest.${MACHINE} ntest.x68k ntest.amd64-8 *.o *.core
