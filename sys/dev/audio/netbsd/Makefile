
SRCS+=	\
	audio_dev.c

SRCS+=	\
	../aucodec.c \
	../aucodec_linear.c \
	../aucodec_msm6258.c \
	../aucodec_mulaw.c \
	../main.c \
	../synth.c \
	../userland.c \
	compat.c \

OBJS=${SRCS:S,.c,.o,:S,../,,}

CFLAGS=	-O2 -g -std=c99 -I. -I.. \
	-DUSERLAND_TEST \
	-fno-strict-aliasing \
	-Wall \
	-Werror \
	-Wshadow \
	-Wno-format-zero-length \
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
