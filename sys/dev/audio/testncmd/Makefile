.include "../Makefile.test"

all:	ncmd.${MACHINE} ncmd.x68k

ncmd.$(MACHINE):	ncmd.o
	${CXX} -g -o $@ $>

ncmd.x68k: ncmd.cpp
	${CXX.x68k} $(CPPFLAGS) -o $@ $> $(LIBS)

clean:
	rm -f ncmd.${MACHINE} ncmd.x68k *.o *.core
