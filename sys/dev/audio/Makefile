CPPFLAGS+=	-g
CPPFLAGS+=	-Wformat
LIBS+=	-lutil

# 限りなくカーネル内と近いオブジェクトを生成するため。
# 出来上がりオブジェクトをリンクするのとどっちがいいんだか。
CPPFLAGS.ltest=	-I. -Inetbsd \
	-mno-red-zone \
	-mno-mmx \
	-mno-sse \
	-mno-avx \
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

all:	ncmd.${MACHINE} ntest.$(MACHINE) ncmd.x68k ntest.x68k ltest

ncmd.$(MACHINE):	ncmd.o
	$(CC) -g -o $@ $>

ntest.$(MACHINE):	ntest.o
	$(CC) -g -o $@ $> $(LIBS)

ncmd.x68k: ncmd.cpp
	/var/obj/7-1/x68k/tools/bin/m68k--netbsdelf-g++ \
		--sysroot=/var/obj/7-1/x68k/dst \
		$(CPPFLAGS) \
		-o $@ $> $(LIBS)

ntest.x68k:	ntest.cpp
	/var/obj/7-1/x68k/tools/bin/m68k--netbsdelf-g++ \
		--sysroot=/var/obj/7-1/x68k/dst \
		$(CPPFLAGS) \
		-o $@ $> $(LIBS)

ltest:	ltest.c
	$(CC) ${CPPFLAGS} ${CPPFLAGS.ltest} -std=c99 -o $@ $> $(LIBS)

clean:
	rm -rf ncmd.o ntest.o ltest
