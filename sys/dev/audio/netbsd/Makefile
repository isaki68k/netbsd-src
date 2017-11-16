
SRCS+=	\
	audio_dev.c

.PATH:..
SRCS+=	\
	aucodec.c \
	aucodec_linear.c \
	aucodec_msm6258.c \
	aucodec_mulaw.c \
	auintr.c \
	aumix.c \
	main.c \
	synth.c \
	compat.c \

CFLAGS=	-g -std=c99 -I. -I.. \
	-Wall \
	-Werror \
	-Wshadow \
	-Wno-format-zero-length \
	-Wno-unused-variable \
	-Wno-unused-but-set-variable \

a.out:	${SRCS:.c=.o}
	${CC} -g -o $@ $> -lm -pthread

clean:
	rm -rf a.out ${SRCS:.c=.o}
