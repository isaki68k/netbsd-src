/*
 * ローカルテスト。
 * 主にエンコーディング変換とかの実環境では分かりづらいやつ。
 */

#include <inttypes.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#define DIAGNOSTIC 1

#include "audiovar.h"
#include "audiodef.h"
#include "aucodec.c"
#include "mulaw.c"
#include "netbsd/compat.c"

struct testtable {
	const char *name;
	void (*func)(void);
};

int debug;
char testname[100];
char descname[100];
int testcount;
int failcount;
int skipcount;
extern struct testtable testtable[];

void __attribute__((__noreturn__))
usage()
{
	// test は複数列挙できる。
	printf("usage: %s [<options>] {-a | <testname...>}\n", getprogname());
	printf("  -d: debug\n");
	printf(" testname:\n");
	for (int i = 0; testtable[i].name != NULL; i++) {
		printf("\t%s\n", testtable[i].name);
	}
	exit(1);
}

int main(int ac, char *av[])
{
	int c;
	int i;
	int opt_all;

	testname[0] = '\0';
	descname[0] = '\0';

	// global option
	opt_all = 0;
	while ((c = getopt(ac, av, "ad")) != -1) {
		switch (c) {
		 case 'a':
			opt_all = 1;
			break;
		 case 'd':
			debug++;
			break;
		 default:
			usage();
		}
	}
	ac -= optind;
	av += optind;

	if (opt_all) {
		// -a なら引数なしで、全項目テスト
		if (ac > 0)
			usage();

		for (int j = 0; testtable[j].name != NULL; j++) {
			testtable[j].func();
			testname[0] = '\0';
			descname[0] = '\0';
		}
	} else {
		// -a なしなら test
		if (ac == 0)
			usage();

		// そうでなければ指定されたやつ(前方一致)を順にテスト
		for (i = 0; i < ac; i++) {
			bool found = false;
			for (int j = 0; testtable[j].name != NULL; j++) {
				if (strncmp(av[i], testtable[j].name, strlen(av[i])) == 0) {
					found = true;
					testtable[j].func();
					testname[0] = '\0';
					descname[0] = '\0';
				}
			}
			if (found == false) {
				printf("test not found: %s\n", av[i]);
				exit(1);
			}
		}
	}
	if (testcount > 0) {
		printf("Result: %d tests, %d success",
			testcount, testcount - failcount);
		if (failcount > 0)
			printf(", %d failed", failcount);
		if (skipcount > 0)
			printf(" (, %d skipped)", skipcount);
		printf("\n");
	}
	return 0;
}


// テスト名
static inline void TEST(const char *, ...) __printflike(1, 2);
static inline void
TEST(const char *name, ...)
{
	va_list ap;

	va_start(ap, name);
	vsnprintf(testname, sizeof(testname), name, ap);
	va_end(ap);
	printf("%s\n", testname);
	fflush(stdout);

	descname[0] = '\0';
}

// テスト詳細
static inline void DESC(const char *, ...) __printflike(1, 2);
static inline void
DESC(const char *name, ...)
{
	va_list ap;

	va_start(ap, name);
	vsnprintf(descname, sizeof(descname), name, ap);
	va_end(ap);
}

// 検査
#define XP_FAIL(fmt...)	xp_fail(__LINE__, fmt)
void xp_fail(int line, const char *fmt, ...)
{
	va_list ap;

	printf(" FAIL %d: %s", line, testname);
	if (descname[0])
		printf("(%s)", descname);
	printf(": ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	failcount++;
}
#define XP_SKIP(fmt...)	xp_skip(__LINE__, fmt)
void xp_skip(int line, const char *fmt, ...)
{
	va_list ap;

	printf(" SKIP %d: %s", line, testname);
	if (descname[0])
		printf("(%s)", descname);
	printf(": ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	skipcount++;
}

#define XP_EQ(exp, act)	xp_eq(__LINE__, exp, act, #act)
void xp_eq(int line, int exp, int act, const char *varname)
{
	testcount++;
	if (exp != act)
		xp_fail(line, "%s expects %d but %d", varname, exp, act);
}

#define XP_NE(exp, act)	xp_ne(__LINE__, exp, act, #act)
void xp_ne(int line, int exp, int act, const char *varname)
{
	testcount++;
	if (exp == act)
		xp_fail(line, "%s expects != %d but %d", varname, exp, act);
}

// これだけ audio.c 内にあって、そのためだけにリンクするのもつらいので
// コピーして持っておく。なんだかなあ。
bool
audio_filter_arg_is_valid(const audio_filter_arg_t *arg)
{
	KASSERT(arg != NULL);

	KASSERT(arg->src != NULL);
	KASSERT(arg->dst != NULL);
	if (!audio_format2_is_valid(arg->srcfmt)) {
		printf("%s: invalid srcfmt\n", __func__);
		return false;
	}
	if (!audio_format2_is_valid(arg->dstfmt)) {
		printf("%s: invalid dstfmt\n", __func__);
		return false;
	}
	if (arg->count <= 0) {
		printf("%s: count(%d) < 0\n", __func__, arg->count);
		return false;
	}
	return true;
}

bool
enc_is_LE(int enc)
{
	switch (enc) {
	 case AUDIO_ENCODING_SLINEAR_LE:
	 case AUDIO_ENCODING_ULINEAR_LE:
		return true;
	 default:
		return false;
	}
}

bool
enc_is_unsigned(int enc)
{
	switch (enc) {
	 case AUDIO_ENCODING_ULINEAR_LE:
	 case AUDIO_ENCODING_ULINEAR_BE:
		return true;
	 default:
		return false;
	}
}

const char *
encname(int enc)
{
	static char buf[16];
	switch (enc) {
	 case AUDIO_ENCODING_SLINEAR_LE:	return "SLINEAR_LE";
	 case AUDIO_ENCODING_SLINEAR_BE:	return "SLINEAR_BE";
	 case AUDIO_ENCODING_ULINEAR_LE:	return "ULINEAR_LE";
	 case AUDIO_ENCODING_ULINEAR_BE:	return "ULINEAR_BE";
	 default:
		snprintf(buf, sizeof(buf), "enc%d", enc);
		return buf;
	}
}

// audio_linear_to_internal のテスト本体
void
test_linear_to(int enc, int prec, void (*func)(audio_filter_arg_t *))
{
	audio_filter_arg_t arg;
	audio_format2_t srcfmt, intfmt;
	int count = 4;
	uint8_t src[count * 4];
	aint_t dst[count];
	aint_t exp[count];
	int32_t val;
	int stride;
	const uint8_t *s;

	TEST("linear%d_to_internal(%s)", prec, encname(enc));

	stride = prec;
	memset(dst, 0, sizeof(dst));
	for (int i = 0; i < sizeof(src); i++) {
		src[i] = i + 1;
		if (((i / 4) % 2) == 1)
			src[i] |= 0x80;
	}

	// src から答え exp を作成
	s = src;
	for (int i = 0; i < count; i++) {
		// input
		val = 0;
		switch (stride) {
		 case 8:
			val = *s++;
			break;
		 case 16:
			if (enc_is_LE(enc))
				val = le16toh(*(const uint16_t *)s);
			else
				val = be16toh(*(const uint16_t *)s);
			s += 2;
			break;
		 case 24:
			if (enc_is_LE(enc)) {
				val = *s++;
				val |= (*s++) << 8;
				val |= (*s++) << 16;
			} else {
				val = (*s++) << 16;
				val |= (*s++) << 8;
				val |= *s++;
			}
			break;
		 case 32:
			if (enc_is_LE(enc))
				val = le32toh(*(const uint32_t *)s);
			else
				val = be32toh(*(const uint32_t *)s);
			s += 4;
			break;
		}

		// length
		if (stride < AUDIO_INTERNAL_BITS) {
			val <<= AUDIO_INTERNAL_BITS - stride;
		} else if (stride > AUDIO_INTERNAL_BITS) {
			val >>= stride - AUDIO_INTERNAL_BITS;
		}

		// unsigned -> signed
		if (enc_is_unsigned(enc)) {
			val ^= 1U << (AUDIO_INTERNAL_BITS - 1);
		}

		exp[i] = val;
	}

	// 呼び出し
	srcfmt.encoding = enc;
	srcfmt.precision = prec;
	srcfmt.stride = prec;
	srcfmt.channels = 2;
	srcfmt.sample_rate = 48000;
	intfmt.encoding = AUDIO_ENCODING_SLINEAR_NE;
	intfmt.precision = AUDIO_INTERNAL_BITS;
	intfmt.stride = AUDIO_INTERNAL_BITS;
	intfmt.channels = 2;
	intfmt.sample_rate = 48000;
	// src を dst に変換して dst と exp を比較。
	arg.srcfmt = &srcfmt;
	arg.dstfmt = &intfmt;
	arg.src = src;
	arg.dst = dst;
	arg.count = count / intfmt.channels;
	func(&arg);

	// 照合
	for (int i = 0; i < count; i++) {
		testcount++;
		if (exp[i] != dst[i]) {
			uint16_t uexp = exp[i];
			uint16_t udst = dst[i];
			xp_fail(__LINE__, "dst[%d] expects %0*x but %0*x", i,
				AUDIO_INTERNAL_BITS / 8, uexp,
				AUDIO_INTERNAL_BITS / 8, udst);
		}
	}
}


void
test_audio_linear8_to_internal()
{
	// linear8 は _LE でも _BE でもないが、
	// どっちにあわせるんだっけ
	test_linear_to(AUDIO_ENCODING_SLINEAR_LE, 8, audio_linear8_to_internal);
	test_linear_to(AUDIO_ENCODING_SLINEAR_BE, 8, audio_linear8_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_LE, 8, audio_linear8_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_BE, 8, audio_linear8_to_internal);
}

void
test_audio_linear16_to_internal()
{
	test_linear_to(AUDIO_ENCODING_SLINEAR_LE, 16, audio_linear16_to_internal);
	test_linear_to(AUDIO_ENCODING_SLINEAR_BE, 16, audio_linear16_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_LE, 16, audio_linear16_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_BE, 16, audio_linear16_to_internal);
}

void
test_audio_linear24_to_internal()
{
#if defined(AUDIO_SUPPORT_LINEAR24)
	test_linear_to(AUDIO_ENCODING_SLINEAR_LE, 24, audio_linear24_to_internal);
	test_linear_to(AUDIO_ENCODING_SLINEAR_BE, 24, audio_linear24_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_LE, 24, audio_linear24_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_BE, 24, audio_linear24_to_internal);
#else
	TEST("audio_linear24_to_internal");
	XP_SKIP("AUDIO_SUPPORT_LINEAR24 not defined");
#endif
}

void
test_audio_linear32_to_internal()
{
	test_linear_to(AUDIO_ENCODING_SLINEAR_LE, 32, audio_linear32_to_internal);
	test_linear_to(AUDIO_ENCODING_SLINEAR_BE, 32, audio_linear32_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_LE, 32, audio_linear32_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_BE, 32, audio_linear32_to_internal);
}

// audio_internal_to_linear* のテスト本体
void
test_to_linear(int enc, int prec, void (*func)(audio_filter_arg_t *))
{
	audio_filter_arg_t arg;
	audio_format2_t intfmt, dstfmt;
	int count = 4;
	aint_t src[count];
	uint8_t exp[count * 4];
	uint8_t dst[count * 4];
	uint32_t val;
	int stride;
	uint8_t *e;

	TEST("audio_internal_to_linear%d(%s)", prec, encname(enc));

	stride = prec;
	memset(dst, 0, sizeof(dst));
	for (int i = 0; i < count; i++) {
		src[i] = i + 1;
		if (i % 2 == 1)
			src[i] |= 1U << (AUDIO_INTERNAL_BITS - 1);
	}

	// src から答え exp を作成
	e = exp;
	for (int i = 0; i < count; i++) {
		if (enc_is_unsigned(enc)) {
			// unsigned -> signed
			val = (int32_t)(aint_t)(src[i] ^ (1U << (AUDIO_INTERNAL_BITS - 1)));
		} else {
			val = src[i];
		}

		// length
		if (stride < AUDIO_INTERNAL_BITS) {
			val >>= AUDIO_INTERNAL_BITS - stride;
		} else if (stride > AUDIO_INTERNAL_BITS) {
			val <<= stride - AUDIO_INTERNAL_BITS;
		}

		// output
		if (enc_is_LE(enc)) {
			*e++ = val & 0xff;
			if (stride > 8) {
				*e++ = (val >> 8) & 0xff;
				if (stride > 16) {
					*e++ = (val >> 16) & 0xff;
					if (stride > 24) {
						*e++ = (val >> 24) & 0xff;
					}
				}
			}
		} else {
			switch (stride) {
			 case 32: *e++ = (val >> 24) & 0xff;
			 case 24: *e++ = (val >> 16) & 0xff;
			 case 16: *e++ = (val >>  8) & 0xff;
			 case  8: *e++ = (val      ) & 0xff;
			}
		}
	}

	// 呼び出し
	intfmt.encoding = AUDIO_ENCODING_SLINEAR_NE;
	intfmt.precision = AUDIO_INTERNAL_BITS;
	intfmt.stride = AUDIO_INTERNAL_BITS;
	intfmt.channels = 2;
	intfmt.sample_rate = 48000;
	dstfmt.encoding = enc;
	dstfmt.precision = prec;
	dstfmt.stride = prec;
	dstfmt.channels = 2;
	dstfmt.sample_rate = 48000;
	arg.srcfmt = &intfmt;
	arg.dstfmt = &dstfmt;
	arg.src = src;
	arg.dst = dst;
	arg.count = count / intfmt.channels;
	func(&arg);

	// 照合
	for (int i = 0; i < count * prec / 8; i++) {
		testcount++;
		if (exp[i] != dst[i]) {
			xp_fail(__LINE__, "dst[%d] expects %02x but %02x", i,
			    exp[i], dst[i]);
		}
	}
}

void
test_audio_internal_to_linear8()
{
	test_to_linear(AUDIO_ENCODING_SLINEAR_LE, 8, audio_internal_to_linear8);
	test_to_linear(AUDIO_ENCODING_SLINEAR_BE, 8, audio_internal_to_linear8);
	test_to_linear(AUDIO_ENCODING_ULINEAR_LE, 8, audio_internal_to_linear8);
	test_to_linear(AUDIO_ENCODING_ULINEAR_BE, 8, audio_internal_to_linear8);
}

void
test_audio_internal_to_linear16()
{
	test_to_linear(AUDIO_ENCODING_SLINEAR_LE, 16, audio_internal_to_linear16);
	test_to_linear(AUDIO_ENCODING_SLINEAR_BE, 16, audio_internal_to_linear16);
	test_to_linear(AUDIO_ENCODING_ULINEAR_LE, 16, audio_internal_to_linear16);
	test_to_linear(AUDIO_ENCODING_ULINEAR_BE, 16, audio_internal_to_linear16);
}

void
test_audio_internal_to_linear24()
{
#if defined(AUDIO_SUPPORT_LINEAR24)
	test_to_linear(AUDIO_ENCODING_SLINEAR_LE, 24, audio_internal_to_linear24);
	test_to_linear(AUDIO_ENCODING_SLINEAR_BE, 24, audio_internal_to_linear24);
	test_to_linear(AUDIO_ENCODING_ULINEAR_LE, 24, audio_internal_to_linear24);
	test_to_linear(AUDIO_ENCODING_ULINEAR_BE, 24, audio_internal_to_linear24);
#else
	TEST("audio_internal_to_linear24");
	XP_SKIP("AUDIO_SUPPORT_LINEAR24 not defined");
#endif
}

void
test_audio_internal_to_linear32()
{
	test_to_linear(AUDIO_ENCODING_SLINEAR_LE, 32, audio_internal_to_linear32);
	test_to_linear(AUDIO_ENCODING_SLINEAR_BE, 32, audio_internal_to_linear32);
	test_to_linear(AUDIO_ENCODING_ULINEAR_LE, 32, audio_internal_to_linear32);
	test_to_linear(AUDIO_ENCODING_ULINEAR_BE, 32, audio_internal_to_linear32);
}

// slinear14
// rawvalue   +33/-32 offset
// 8158..4063 (8191..4096), 16 interval of 256	= 0x80 + interval
// 4062..2015 (4095..2048), 16 interval of 128	= 0x90 + interval
// 2014.. 991 (2047..1024), 16 interval of  64	= 0xa0 + interval
//  991.. 479 (1023.. 512), 16 interval of  32	= 0xb0 + interval
//  478.. 223 ( 511.. 256), 16 interval of  16	= 0xc0 + interval
//  222..  95 ( 255.. 128), 16 interval of   8	= 0xd0 + interval
//   94..  31 ( 127..  64), 16 interval of   4	= 0xe0 + interval
//   30..   1 (  63..  34), 15 interval of   2	= 0xf0 + interval
// (         ->  63..  32 , 16 interval)
//    0                                      	= 0xff

// 32764..16384 = 0x7ffc..0x4000, 1024x16 = 80+
//      .. 8192,  512x16 = 90+
//      .. 4096,  256x16 = a0+
//      .. 2048,  128x16 = b0+
//      .. 1024,   64x16 = c0+
//      ..  512,   32x16 = d0+
//      ..  256,   16x16 = e0+
//      ..  136,    8x15 = f0+
// 0                     = ff
// -1                    = 7f
//   -64..  -35,    8x15 = 70+
//  -128..  -65

void
test_audio_mulaw_to_internal()
{
	audio_filter_arg_t arg;
	audio_format2_t srcfmt, intfmt;
	int count = 256;
	uint8_t src[count];
	aint_t dst[count];
	aint_t exp[count];

	TEST("audio_mulaw_to_internal");

	memset(dst, 0, sizeof(dst));
	for (int i = 0; i < __arraycount(src); i++) {
		// src を作成
		src[i] = (int8_t)(uint8_t)i;

		// src から答え exp を作成
		int x;
		int b, off;
		char m = i;
		if (m < (char)0x90)			b = 8158, off = 256;
		else if (m < (char)0xa0)	b = 4062, off = 128;
		else if (m < (char)0xb0)	b = 2014, off = 64;
		else if (m < (char)0xc0)	b = 990, off = 32;
		else if (m < (char)0xd0)	b = 478, off = 16;
		else if (m < (char)0xe0)	b = 222, off = 8;
		else if (m < (char)0xf0)	b = 94, off = 4;
		else if (m < (char)0xff)	b = 30, off = 2;
		else if (m == (char)0xff)	b = 0, off = 0;
		else if (m < (char)0x10)	b = -8159, off = 256;
		else if (m < (char)0x20)	b = -4063, off = 128;
		else if (m < (char)0x30)	b = -2015, off = 64;
		else if (m < (char)0x40)	b = -991, off = 32;
		else if (m < (char)0x50)	b = -479, off = 16;
		else if (m < (char)0x60)	b = -223, off = 8;
		else if (m < (char)0x70)	b = -95, off = 4;
		else if (m < (char)0x7f)	b = -31, off = 2;
		else if (m == (char)0x7f)	b = -1, off = 0;

#if 0
		// これが wikipedia の式をそのまま起こしたもの
		if (m < 0)
			x = b - (m & 0xf) * off;
		else
			x = b + (m & 0xf) * off;
#else
		// これが NetBSD7 のテーブル生成式
		if (m < 0)
			x = b - (m & 0xf) * off - (off - 1) / 2;
		else
			x = b + (m & 0xf) * off + (off + 1) / 2;
#endif
		exp[i] = x << 2;
	}

	// 呼び出し
	srcfmt.encoding = AUDIO_ENCODING_ULAW;
	srcfmt.precision = 8;
	srcfmt.stride = 8;
	srcfmt.channels = 1;
	srcfmt.sample_rate = 8000;
	intfmt.encoding = AUDIO_ENCODING_SLINEAR_NE;
	intfmt.precision = AUDIO_INTERNAL_BITS;
	intfmt.stride = AUDIO_INTERNAL_BITS;
	intfmt.channels = 1;
	intfmt.sample_rate = 8000;
	// src を dst に変換して dst と exp を比較。
	arg.srcfmt = &srcfmt;
	arg.dstfmt = &intfmt;
	arg.src = src;
	arg.dst = dst;
	arg.count = count / intfmt.channels;
	audio_mulaw_to_internal(&arg);

	// 照合
	for (int i = 0; i < count; i++) {
		testcount++;
		if (exp[i] != dst[i]) {
			uint16_t uexp = exp[i];
			uint16_t udst = dst[i];
			xp_fail(__LINE__, "dst[0x%02x] expects 0x%0*x(%d) but 0x%0*x(%d)",
				i,
				AUDIO_INTERNAL_BITS / 4, uexp, exp[i] >> 2,
				AUDIO_INTERNAL_BITS / 4, udst, dst[i] >> 2);
		}
	}
}

void
test_audio_internal_to_mulaw()
{
	audio_filter_arg_t arg;
	audio_format2_t intfmt, dstfmt;
	int count = 16384;
	aint_t *src;
	uint8_t *dst;
	uint8_t *exp;
	int hqmode;

	// hqmode なら14ビット全部使って期待値を用意する。定義式通り。
	// そうでなければ上位8ビットだけ使って期待値を用意する。こちらは
	// NetBSD7 までの256バイトテーブル方式との互換性。
#if defined(MULAW_HQ_ENC)
	hqmode = 1;
#else
	hqmode = 0;
#endif

	TEST("audio_internal_to_mulaw[%s]", hqmode ? "14bit" : "8bit");

	src = malloc(count * sizeof(*src));
	dst = malloc(count * sizeof(*dst));
	exp = malloc(count * sizeof(*exp));
	memset(dst, 0, count * sizeof(*dst));
	for (int i = 0; i < count; i++) {
		// src を作成
		src[i] = ((int16_t)(i << 2)) << (AUDIO_INTERNAL_BITS - 16);

		// src から答え exp を作成
		int x;
		int b, off, m;
		x = src[i] >> 2;

		if(debug)printf("i=%d x=%d", i, x);
		if (hqmode == 0) {
			// NetBSD7 は入力を slinear8 とした256段階しか使ってない。
			x >>= 6;
			x <<= 6;
			if(debug)printf("->%d", x);
		}

		if (x > 4062)    	b = 8158,	off = -256, m = 0x80;
		else if (x > 2014)	b = 4062,	off = -128,	m = 0x90;
		else if (x > 990)	b = 2014,	off = -64,	m = 0xa0;
		else if (x > 478)	b = 990,	off = -32,	m = 0xb0;
		else if (x > 222)	b = 478,	off = -16,	m = 0xc0;
		else if (x > 94)	b = 222,	off = -8,	m = 0xd0;
		else if (x > 30)	b = 94, 	off = -4,	m = 0xe0;
		else if (x > 0) 	b = 30, 	off = -2,	m = 0xf0;
		else if (x == 0)	b = 0,		off = 0,	m = 0xff;
		else if (x < -4063)	b = -8159,	off = 256,	m = 0x00;
		else if (x < -2015)	b = -4063,	off = 128,	m = 0x10;
		else if (x < -991)	b = -2015,	off = 64,	m = 0x20;
		else if (x < -479)	b = -991,	off = 32,	m = 0x30;
		else if (x < -223)	b = -479,	off = 16,	m = 0x40;
		else if (x < -95)	b = -223,	off = 8,	m = 0x50;
		else if (x < -31)	b = -95,	off = 4,	m = 0x60;
		else if (x < -1)	b = -31,	off = 2,	m = 0x70;
		else if (x == -1)	b = -1, 	off = 0,	m = 0x7f;

		if(debug)printf(", b=%d off=%d, m=0x%02x", b, off, m);

		int j = 0;
		// wikipedia 通り 14bit
		if (x > 0) {
			for (j = 0; j < 16; j++) {
				if (x > b + off * (j + 1))
					break;
			}
		} else if (x < -1) {
			for (j = 0; j < 16; j++) {
				if (x < b + off * (j + 1))
					break;
			}
		}
		if(debug)printf("\texp=0x%02x\n", m+j);
		exp[i] = m + j;
	}

	// 呼び出し
	dstfmt.encoding = AUDIO_ENCODING_ULAW;
	dstfmt.precision = 8;
	dstfmt.stride = 8;
	dstfmt.channels = 1;
	dstfmt.sample_rate = 8000;
	intfmt.encoding = AUDIO_ENCODING_SLINEAR_NE;
	intfmt.precision = AUDIO_INTERNAL_BITS;
	intfmt.stride = AUDIO_INTERNAL_BITS;
	intfmt.channels = 1;
	intfmt.sample_rate = 8000;
	// src を dst に変換して dst と exp を比較。
	arg.srcfmt = &intfmt;
	arg.dstfmt = &dstfmt;
	arg.src = src;
	arg.dst = dst;
	arg.count = count / intfmt.channels;
	audio_internal_to_mulaw(&arg);

	// 照合
	for (int i = 0; i < count; i++) {
		testcount++;
		if (exp[i] != dst[i]) {
			xp_fail(__LINE__, "dst[0x%02x](%d) expects 0x%02x but 0x%02x",
				i, src[i] >> 2, exp[i], dst[i]);
		}
	}

	free(src);
	free(dst);
	free(exp);
}

// テスト用に ring を用意。
audio_ring_t *
ring_init(int capacity)
{
	audio_ring_t *ring;

	ring = (audio_ring_t *)malloc(sizeof(*ring));
	ring->fmt.encoding = AUDIO_ENCODING_ULAW;
	ring->fmt.precision = 8;
	ring->fmt.stride = 8;
	ring->fmt.channels = 1;
	ring->fmt.sample_rate = 8000;
	ring->capacity = capacity;
	ring->head = 0;
	ring->used = 0;
	ring->mem = malloc(ring->capacity * ring->fmt.stride / NBBY);

	return ring;
}

// テスト用に aint_t の ring を用意。
audio_ring_t *
ring_init_aint(int capacity)
{
	audio_ring_t *ring;

	ring = (audio_ring_t *)malloc(sizeof(*ring));
	ring->fmt.encoding = AUDIO_ENCODING_SLINEAR_NE;
	ring->fmt.precision = AUDIO_INTERNAL_BITS;
	ring->fmt.stride = AUDIO_INTERNAL_BITS;
	ring->fmt.channels = 2;
	ring->fmt.sample_rate = 8000;
	ring->capacity = capacity;
	ring->head = 0;
	ring->used = 0;
	ring->mem = malloc(ring->capacity * ring->fmt.stride / NBBY);

	return ring;
}

void
ring_free(audio_ring_t *ring)
{
	free(ring->mem);
	free(ring);
}

// abort をトラップする
jmp_buf jmp;
void
signal_abort(int signo)
{
	longjmp(jmp, 1);
}

void
test_audio_ring_round()
{
	struct {
		int idx;
		int exp;
	} table[] = {
		// exp==-1 はKASSERTになるケース
		{ 0,	0 },
		{ 1,	1 },
		{ 2,	2 },
		{ 3,	0 },
		{ 4,	1 },
		{ 5,	2 },
		{ 6,	-1 },
	};
	audio_ring_t *ring = ring_init(3);

	TEST("audio_ring_round");
	signal(SIGABRT, signal_abort);

	for (int i = 0; i < __arraycount(table); i++) {
		int idx = table[i].idx;
		int exp = table[i].exp;

		if (exp >= 0) {
			// 正常なケース
			int act = audio_ring_round(ring, idx);
			XP_EQ(exp, act);
		} else {
			// KASSERT になるケース
			panic_msgout = 0;
			int aborted = setjmp(jmp);
			if (aborted == 0) {
				audio_ring_round(ring, idx);
			}
			panic_msgout = 1;
			XP_EQ(1, aborted);
		}
	}

	signal(SIGABRT, SIG_DFL);
	ring_free(ring);
}

void
test_audio_ring_tail()
{
	struct {
		int head;
		int used;
		int exp;
	} table[] = {
		{ 0, 0,		0 },
		{ 0, 1,		1 },
		{ 0, 2,		2 },
		{ 0, 3,		0 },
		{ 1, 0,		1 },
		{ 1, 1,		2 },
		{ 1, 2,		0 },
		{ 1, 3,		1 },
		{ 2, 0,		2 },
		{ 2, 1,		0 },
		{ 2, 2,		1 },
		{ 2, 3,		2 },
	};
	audio_ring_t *ring = ring_init(3);

	TEST("audio_ring_tail");
	for (int i = 0; i < __arraycount(table); i++) {
		int head = table[i].head;
		int used = table[i].used;
		int exp = table[i].exp;

		ring->head = head;
		ring->used = used;
		int act = audio_ring_tail(ring);
		XP_EQ(exp, act);
	}

	ring_free(ring);
}

void
test_audio_ring_headptr_aint()
{
	int capacity = 3;
	audio_ring_t *ring = ring_init_aint(capacity);

	TEST("audio_ring_headptr_aint");
	for (int i = 0; i < capacity; i++) {
		int head = i;
		int exp = i * ring->fmt.channels * ring->fmt.stride / NBBY;

		ring->head = head;
		void *act = audio_ring_headptr_aint(ring);
		XP_EQ(exp, (int)((uint8_t *)act - (uint8_t *)ring->mem));
	}

	ring_free(ring);
}

void
test_audio_ring_tailptr_aint()
{
	int capacity = 3;
	audio_ring_t *ring = ring_init_aint(capacity);

	TEST("audio_ring_tailptr_aint");
	for (int i = 0; i < capacity; i++) {
		int used = i;
		int exp = i * ring->fmt.channels * ring->fmt.stride / NBBY;

		ring->used = used;
		void *act = audio_ring_tailptr_aint(ring);
		XP_EQ(exp, (int)((uint8_t *)act - (uint8_t *)ring->mem));
	}

	ring_free(ring);
}

void
test_audio_ring_headptr()
{
	int capacity = 3;
	audio_ring_t *ring = ring_init(capacity);

	TEST("audio_ring_headptr");
	for (int i = 0; i < capacity; i++) {
		int head = i;
		int exp = i * ring->fmt.channels * ring->fmt.stride / NBBY;

		ring->head = head;
		void *act = audio_ring_headptr(ring);
		XP_EQ(exp, (int)((uint8_t *)act - (uint8_t *)ring->mem));
	}

	ring_free(ring);
}

void
test_audio_ring_tailptr()
{
	int capacity = 3;
	audio_ring_t *ring = ring_init(capacity);

	TEST("audio_ring_tailptr");
	for (int i = 0; i < capacity; i++) {
		int used = i;
		int exp = i * ring->fmt.channels * ring->fmt.stride / NBBY;

		ring->used = used;
		void *act = audio_ring_tailptr(ring);
		XP_EQ(exp, (int)((uint8_t *)act - (uint8_t *)ring->mem));
	}

	ring_free(ring);
}

void
test_audio_ring_bytelen()
{
	TEST("audio_ring_bytelen");

	for (int i = 1; i < 3; i++) {
		audio_ring_t *ring = ring_init(i);
		int exp = ring->capacity * ring->fmt.channels * ring->fmt.stride / NBBY;
		XP_EQ(exp, audio_ring_bytelen(ring));
		ring_free(ring);
	}

	for (int i = 1; i < 3; i++) {
		audio_ring_t *ring = ring_init_aint(i);
		int exp = ring->capacity * ring->fmt.channels * ring->fmt.stride / NBBY;
		XP_EQ(exp, audio_ring_bytelen(ring));
		ring_free(ring);
	}
}

void
test_audio_ring_take()
{
	struct {
		int head;
		int used;
		int take;
		bool result;
		int exphead;
		int expused;
	} table[] = {
#define FALSE	false, 0, 0
		//hd us tk	res  期待するhead,used
		{ 0, 0, 0,	true, 0, 0 },
		{ 0, 0, 1,	FALSE },
		{ 0, 0, 2,	FALSE },
		{ 0, 0, 3,	FALSE },
		{ 0, 1, 0,	true, 0, 1 },
		{ 0, 1, 1,	true, 1, 0 },
		{ 0, 1, 2,	FALSE },
		{ 0, 1, 3,	FALSE },
		{ 0, 2, 0,	true, 0, 2 },
		{ 0, 2, 1,	true, 1, 1 },
		{ 0, 2, 2,	true, 2, 0 },
		{ 0, 2, 3,	FALSE },
		{ 0, 3, 0,	true, 0, 3 },
		{ 0, 3, 1,	true, 1, 2 },
		{ 0, 3, 2,	true, 2, 1 },
		{ 0, 3, 3,	true, 0, 0 },

		{ 1, 0, 0,	true, 1, 0 },
		{ 1, 0, 1,	FALSE },
		{ 1, 0, 2,	FALSE },
		{ 1, 0, 3,	FALSE },
		{ 1, 1, 0,	true, 1, 1 },
		{ 1, 1, 1,	true, 2, 0 },
		{ 1, 1, 2,	FALSE },
		{ 1, 1, 3,	FALSE },
		{ 1, 2, 0,	true, 1, 2 },
		{ 1, 2, 1,	true, 2, 1 },
		{ 1, 2, 2,	true, 0, 0 },
		{ 1, 2, 3,	FALSE },
		{ 1, 3, 0,	true, 1, 3 },
		{ 1, 3, 1,	true, 2, 2 },
		{ 1, 3, 2,	true, 0, 1 },
		{ 1, 3, 3,	true, 1, 0 },

		{ 2, 0, 0,	true, 2, 0 },
		{ 2, 0, 1,	FALSE },
		{ 2, 0, 2,	FALSE },
		{ 2, 0, 3,	FALSE },
		{ 2, 1, 0,	true, 2, 1 },
		{ 2, 1, 1,	true, 0, 0 },
		{ 2, 1, 2,	FALSE },
		{ 2, 1, 3,	FALSE },
		{ 2, 2, 0,	true, 2, 2 },
		{ 2, 2, 1,	true, 0, 1 },
		{ 2, 2, 2,	true, 1, 0 },
		{ 2, 2, 3,	FALSE },
		{ 2, 3, 0,	true, 2, 3 },
		{ 2, 3, 1,	true, 0, 2 },
		{ 2, 3, 2,	true, 1, 1 },
		{ 2, 3, 3,	true, 2, 0 },
#undef FALSE
	};
	audio_ring_t *ring = ring_init(3);

	TEST("audio_ring_take");
	signal(SIGABRT, signal_abort);

	for (int i = 0; i < __arraycount(table); i++) {
		int head = table[i].head;
		int used = table[i].used;
		int take = table[i].take;
		bool result = table[i].result;
		int exphead = table[i].exphead;
		int expused = table[i].expused;
		//TEST("audio_ring_take(%d,%d,%d)", head, used, take);

		ring->head = head;
		ring->used = used;

		if (result) {
			// 正常なケース
			audio_ring_take(ring, take);
			XP_EQ(exphead, ring->head);
			XP_EQ(expused, ring->used);
		} else {
			// KASSERT になるケース
			panic_msgout = 0;
			int aborted = setjmp(jmp);
			if (aborted == 0) {
				audio_ring_take(ring, take);
			}
			panic_msgout = 1;
			XP_EQ(1, aborted);
		}
	}

	signal(SIGABRT, SIG_DFL);
	ring_free(ring);
}

void
test_audio_ring_push()
{
	struct {
		int head;
		int used;
		int push;
		bool result;
		int exphead;
		int expused;
	} table[] = {
#define FALSE	false, 0, 0
		//hd us ps	res  期待するhead,used
		{ 0, 0, 0,	true, 0, 0 },
		{ 0, 0, 1,	true, 0, 1 },
		{ 0, 0, 2,	true, 0, 2 },
		{ 0, 0, 3,	true, 0, 3 },
		{ 0, 1, 0,	true, 0, 1 },
		{ 0, 1, 1,	true, 0, 2 },
		{ 0, 1, 2,	true, 0, 3 },
		{ 0, 1, 3,	FALSE },
		{ 0, 2, 0,	true, 0, 2 },
		{ 0, 2, 1,	true, 0, 3 },
		{ 0, 2, 2,	FALSE },
		{ 0, 2, 3,	FALSE },
		{ 0, 3, 0,	true, 0, 3 },
		{ 0, 3, 1,	FALSE },
		{ 0, 3, 2,	FALSE },
		{ 0, 3, 3,	FALSE },

		{ 1, 0, 0,	true, 1, 0 },
		{ 1, 0, 1,	true, 1, 1 },
		{ 1, 0, 2,	true, 1, 2 },
		{ 1, 0, 3,	true, 1, 3 },
		{ 1, 1, 0,	true, 1, 1 },
		{ 1, 1, 1,	true, 1, 2 },
		{ 1, 1, 2,	true, 1, 3 },
		{ 1, 1, 3,	FALSE },
		{ 1, 2, 0,	true, 1, 2 },
		{ 1, 2, 1,	true, 1, 3 },
		{ 1, 2, 2,	FALSE },
		{ 1, 2, 3,	FALSE },
		{ 1, 3, 0,	true, 1, 3 },
		{ 1, 3, 1,	FALSE },
		{ 1, 3, 2,	FALSE },
		{ 1, 3, 3,	FALSE },

		{ 2, 0, 0,	true, 2, 0 },
		{ 2, 0, 1,	true, 2, 1 },
		{ 2, 0, 2,	true, 2, 2 },
		{ 2, 0, 3,	true, 2, 3 },
		{ 2, 1, 0,	true, 2, 1 },
		{ 2, 1, 1,	true, 2, 2 },
		{ 2, 1, 2,	true, 2, 3 },
		{ 2, 1, 3,	FALSE },
		{ 2, 2, 0,	true, 2, 2 },
		{ 2, 2, 1,	true, 2, 3 },
		{ 2, 2, 2,	FALSE },
		{ 2, 2, 3,	FALSE },
		{ 2, 3, 0,	true, 2, 3 },
		{ 2, 3, 1,	FALSE },
		{ 2, 3, 2,	FALSE },
		{ 2, 3, 3,	FALSE },
#undef FALSE
	};
	audio_ring_t *ring = ring_init(3);

	TEST("audio_ring_push");
	signal(SIGABRT, signal_abort);

	for (int i = 0; i < __arraycount(table); i++) {
		int head = table[i].head;
		int used = table[i].used;
		int push = table[i].push;
		bool result = table[i].result;
		int exphead = table[i].exphead;
		int expused = table[i].expused;
		//TEST("audio_ring_push(%d,%d,%d)", head, used, push);

		ring->head = head;
		ring->used = used;

		if (result) {
			// 正常なケース
			audio_ring_push(ring, push);
			XP_EQ(exphead, ring->head);
			XP_EQ(expused, ring->used);
		} else {
			// KASSERT になるケース
			panic_msgout = 0;
			int aborted = setjmp(jmp);
			if (aborted == 0) {
				audio_ring_push(ring, push);
			}
			panic_msgout = 1;
			XP_EQ(1, aborted);
		}
	}

	signal(SIGABRT, SIG_DFL);
	ring_free(ring);
}

void
test_audio_ring_get_contig_used()
{
	struct {
		int head;
		int used;
		int exp;
	} table[] = {
		{ 0, 0,		0 },
		{ 0, 1,		1 },
		{ 0, 2,		2 },
		{ 0, 3,		3 },
		{ 1, 0,		0 },
		{ 1, 1,		1 },
		{ 1, 2,		2 },
		{ 1, 3,		2 },
		{ 2, 0,		0 },
		{ 2, 1,		1 },
		{ 2, 2,		1 },
		{ 2, 3,		1 },
	};
	audio_ring_t *ring = ring_init(3);

	TEST("audio_ring_get_contig_used");
	for (int i = 0; i < __arraycount(table); i++) {
		int head = table[i].head;
		int used = table[i].used;
		int exp = table[i].exp;
		//TEST("audio_ring_get_contig_used(%d,%d)", head, used);

		ring->head = head;
		ring->used = used;
		XP_EQ(exp, audio_ring_get_contig_used(ring));
	}

	ring_free(ring);
}

void
test_audio_ring_get_contig_free()
{
	struct {
		int head;
		int used;
		int exp;
	} table[] = {
		{ 0, 0,		3 },
		{ 0, 1,		2 },
		{ 0, 2,		1 },
		{ 0, 3,		0 },
		{ 1, 0,		2 },
		{ 1, 1,		1 },
		{ 1, 2,		1 },
		{ 1, 3,		0 },
		{ 2, 0,		1 },
		{ 2, 1,		2 },
		{ 2, 2,		1 },
		{ 2, 3,		0 },
	};
	audio_ring_t *ring = ring_init(3);

	TEST("audio_ring_get_contig_free");
	for (int i = 0; i < __arraycount(table); i++) {
		int head = table[i].head;
		int used = table[i].used;
		int exp = table[i].exp;
		//TEST("audio_ring_get_contig_free(%d,%d)", head, used);

		ring->head = head;
		ring->used = used;
		XP_EQ(exp, audio_ring_get_contig_free(ring));
	}

	ring_free(ring);
}

// テスト一覧
#define DEF(x)	{ #x, test_ ## x }
struct testtable testtable[] = {
	DEF(audio_linear8_to_internal),
	DEF(audio_linear16_to_internal),
	DEF(audio_linear24_to_internal),
	DEF(audio_linear32_to_internal),
	DEF(audio_internal_to_linear8),
	DEF(audio_internal_to_linear16),
	DEF(audio_internal_to_linear24),
	DEF(audio_internal_to_linear32),
	DEF(audio_mulaw_to_internal),
	DEF(audio_internal_to_mulaw),
	DEF(audio_ring_round),
	DEF(audio_ring_tail),
	DEF(audio_ring_headptr_aint),
	DEF(audio_ring_tailptr_aint),
	DEF(audio_ring_headptr),
	DEF(audio_ring_tailptr),
	DEF(audio_ring_bytelen),
	DEF(audio_ring_take),
	DEF(audio_ring_push),
	DEF(audio_ring_get_contig_used),
	DEF(audio_ring_get_contig_free),
	{ NULL, NULL },
};
