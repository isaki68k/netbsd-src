.include "../Makefile.test"

CPPFLAGS+=	-I../testn
LIBS+=	-lossaudio

all:	testoss.${MACHINE} testoss.${MACHINE}-9

testoss.${MACHINE}:	testoss.cpp ../testn/xptest.cpp
	${CXX.${MACHINE}} ${CPPFLAGS} -o $@ testoss.cpp ${LIBS}

testoss.${MACHINE}-9: testoss.cpp ../testn/xptest.cpp
	${CXX} ${CPPFLAGS} -o $@ testoss.cpp ${LIBS}

clean:
	rm -f testoss.${MACHINE} *.o *.core
