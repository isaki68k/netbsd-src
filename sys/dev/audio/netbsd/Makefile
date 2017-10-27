
SRCS+=	\
	audio_dev.c

.PATH:..
SRCS+=	\
	aucodec.c \
	aucodec_linear.c \
	aucodec_msm6258.c \
	aucodec_mulaw.c \
	aumix.c \
	main.c \
	synth.c \

CFLAGS=	-g -std=c99 -I. -I.. \
	-DUSE_PTHREAD \
	-Wall \
	-Werror \
	-Wshadow \

a.out:	${SRCS:.c=.o}
	${CC} -g -o $@ $> -lm -pthread

clean:
	rm -rf a.out ${SRCS:.c=.o}
