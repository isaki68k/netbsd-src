SRCS+=	\
	main.c \
	audio_dev.c \
	userland.c \
	compat.c \

OBJS=${SRCS:S/.c/.o/}

CFLAGS=	-O2 -g -std=c99 -I. -I.. \
	-DNAUDIO=1 \
	-DAUDIO2 \
	-DSTATIC=extern \
	-fno-strict-aliasing \
	-Wall \
	-Werror \
	-Wshadow \
	-Wno-format-zero-length \
	-Wno-unused-function \
	-Wno-unused-variable \
	-Wno-unused-but-set-variable \

all:	testplay.${MACHINE}

testplay.${MACHINE}:	${OBJS}
	${CC} -g -o $@ $> -lm -pthread -lutil

.PHONY: clean
clean:
	rm -f testplay.* *.o *.core

.PHONY: depend
depend:
	${CC} -MM ${CFLAGS} ${SRCS} > .depend

.if exists(.depend)
.include ".depend"
.endif
