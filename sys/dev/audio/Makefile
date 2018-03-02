CC.x68k=	/var/obj/7-1/x68k/tools/bin/m68k--netbsdelf-gcc \
		--sysroot=/var/obj/7-1/x68k/dst
CXX.x68k=	/var/obj/7-1/x68k/tools/bin/m68k--netbsdelf-g++ \
		--sysroot=/var/obj/7-1/x68k/dst
CPPFLAGS+=	-g
CPPFLAGS+=	-Wformat
LIBS+=	-lutil

# 限りなくカーネル内と近いオブジェクトを生成するため。
# 出来上がりオブジェクトをリンクするのとどっちがいいんだか。
CPPFLAGS.amd64= \
	-mno-red-zone \
	-mno-mmx \
	-mno-sse \
	-mno-avx \

CPPFLAGS.x68k= \
	-m68020-60 \
	-Wa,-m68030 \
	-Wa,-m68851 \
	-ffreestanding \
	-fno-reorder-blocks \

CPPFLAGS.ltest=	-I. -Inetbsd \
	-msoft-float \
	-fno-zero-initialized-in-bss \
	-O2 \
	-fno-omit-frame-pointer \
	-fstack-protector \
	-Wstack-protector \
	--param ssp-buffer-size=1 \
	-fno-strict-aliasing \
	-fno-common \
	-std=gnu99 \
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
	-DAUDIO2 \

# テストコードにはきつすぎる
#	-Wmissing-prototypes \
#	-Wstrict-prototypes \
#	-Wold-style-definition \

all:	ncmd.${MACHINE} ntest.$(MACHINE) ncmd.x68k ntest.x68k ltest ltest.x68k

ncmd.$(MACHINE):	ncmd.o
	$(CC) -g -o $@ $>

ntest.$(MACHINE):	ntest.o
	$(CC) -g -o $@ $> $(LIBS)

ncmd.x68k: ncmd.cpp
	${CXX.x68k} $(CPPFLAGS) -o $@ $> $(LIBS)

ntest.x68k:	ntest.cpp
	${CXX.x68k} $(CPPFLAGS) -o $@ $> $(LIBS)

ltest:	ltest.c
	$(CC) ${CPPFLAGS} ${CPPFLAGS.amd64} ${CPPFLAGS.ltest} -std=c99 -o $@ $> $(LIBS)

ltest.x68k:	ltest.c
	$(CC.x68k) ${CPPFLAGS} ${CPPFLAGS.x68k} ${CPPFLAGS.ltest} -std=c99 -o $@ $> $(LIBS)

clean:
	rm -rf ncmd.o ntest.o ltest ltest.x68k
