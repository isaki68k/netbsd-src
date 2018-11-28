.include "../Makefile.test"

all:	testmulaw.c
	${CC} ${CPPFLAGS} -I.. -I../testplay testmulaw.c -o testmulaw

clean:
	rm -f testmulaw *.o *.core
