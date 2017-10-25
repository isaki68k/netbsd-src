
SRCS+=	\
	audio_dev.c

.PATH:..
SRCS+=	\
	aucodec.c \
	aucodec_linear.c \
	aucodec_msm6258.c \
	aucodec_mulaw.c \
	aumix.c \

.PATH:../win
SRCS+=	\
	audio_test.c \

CFLAGS=	-g -std=c99 -I. -I.. \
	-Wall \
	-Werror \
	-Wshadow \

a.out:	${SRCS:.c=.o}
	${CC} -g -o $@ $> -lm

clean:
	rm -rf a.out ${SRCS:.c=.o}
