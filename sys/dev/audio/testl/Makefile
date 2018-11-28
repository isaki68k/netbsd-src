CPPFLAGS+=	\
	-I. -I.. -I../testplay \
	-msoft-float \
	-fno-zero-initialized-in-bss \
	-O2 \
	-fno-omit-frame-pointer \
	--param ssp-buffer-size=1 \
	-fno-strict-aliasing \
	-fno-common \
	-Werror \
	-Wall \
	-Wno-format-zero-length \
	-Wpointer-arith \
	-Wswitch \
	-Wshadow \
	-Wcast-qual \
	-Wwrite-strings \
	-Wno-unreachable-code \
	-Wno-pointer-sign \
	-Wno-attributes \
	-Wextra \
	-Wno-unused-parameter \
	-Wno-sign-compare \
	-DNAUDIO=1 \
	-DAUDIO2 \
	-DDIAGNOSTIC=1 \
	-Wno-format-zero-length \
	-Wno-unused-function \
	-Wno-unused-variable \
	-Wno-unused-but-set-variable \
	-Wno-missing-field-initializers \

.include "../Makefile.test"

all:	ltest ltest.x68k

ltest:	ltest.c
	$(CC) ${CPPFLAGS} ${CPPFLAGS.amd64} -std=c99 -o $@ $> $(LIBS)

ltest.x68k:	ltest.c
	$(CC.x68k) ${CPPFLAGS} ${CPPFLAGS.x68k} -std=c99 -o $@ $> $(LIBS)

clean:
	rm -f ltest ltest.x68k *.o *.core

