
SRCS+=	\
	audio_dev.c

SRCS+=	\
	../main.c \
	../mulaw.c \
	../userland.c \
	compat.c \

OBJS=${SRCS:S,.c,.o,:S,../,,}

CFLAGS=	-O2 -g -std=c99 -I. -I.. \
	-DNAUDIO=1 \
	-DAUDIO2 \
	-fno-strict-aliasing \
	-Wall \
	-Werror \
	-Wshadow \
	-Wno-format-zero-length \
	-Wno-unused-function \
	-Wno-unused-variable \
	-Wno-unused-but-set-variable \

a.out.${MACHINE}:	${OBJS}
	${CC} -g -o $@ $> -lm -pthread

.PHONY: clean
clean:
	rm -rf a.out.* *.o

.PHONY: depend
depend:
	${CC} -MM ${CFLAGS} ${SRCS} > .depend

.if exists(.depend)
.include ".depend"
.endif
