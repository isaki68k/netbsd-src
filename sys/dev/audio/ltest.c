/*
 * ローカルテスト。
 * 主にエンコーディング変換とかの実環境では分かりづらいやつ。
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "audiovar.h"
#include "aucodec.h"
#include "auformat.h"
#include "uio.h"
#include "aucodec_linear.c"

struct testtable {
	const char *name;
	void (*func)(void);
};

int debug;
char testname[100];
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
}

// 検査
#define XP_FAIL(fmt...)	xp_fail(__LINE__, fmt)
void xp_fail(int line, const char *fmt, ...)
{
	va_list ap;

	printf(" FAIL %d: %s ", line, testname);
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

	printf(" SKIP %d: ", line);
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

// これだけ aumix.c 内にあって、そのためだけにリンクするのもつらいので
// コピーして持っておく。なんだかなあ。
bool
is_valid_filter_arg(const audio_filter_arg_t *arg)
{
	KASSERT(arg != NULL);

	KASSERT(arg->src != NULL);
	KASSERT(arg->dst != NULL);
	if (!is_valid_format(arg->srcfmt)) {
		printf("%s: invalid srcfmt\n", __func__);
		return false;
	}
	if (!is_valid_format(arg->dstfmt)) {
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

// linear_to_internal のテスト本体
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

		// length (internal = 16bit)
		switch (stride) {
		 case 8:
			val <<= 8;
			break;
		 case 16:
			break;
		 case 24:
			val >>= 8;
			break;
		 case 32:
			val >>= 16;
			break;
		}

		// unsigned -> signed
		if (enc_is_unsigned(enc)) {
			val ^= 0x8000;
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
			xp_fail(__LINE__, "dst[%d] expects %04x but %04x", i,
				uexp, udst);
		}
	}
}


void
test_linear8_to_internal()
{
	// linear8 は _LE でも _BE でもないが、
	// どっちにあわせるんだっけ
	test_linear_to(AUDIO_ENCODING_SLINEAR_LE, 8, linear8_to_internal);
	test_linear_to(AUDIO_ENCODING_SLINEAR_BE, 8, linear8_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_LE, 8, linear8_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_BE, 8, linear8_to_internal);
}

void
test_linear16_to_internal()
{
	test_linear_to(AUDIO_ENCODING_SLINEAR_LE, 16, linear16_to_internal);
	test_linear_to(AUDIO_ENCODING_SLINEAR_BE, 16, linear16_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_LE, 16, linear16_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_BE, 16, linear16_to_internal);
}

void
test_linear24_to_internal()
{
#if defined(AUDIO_SUPPORT_LINEAR24)
	test_linear_to(AUDIO_ENCODING_SLINEAR_LE, 24, linear24_to_internal);
	test_linear_to(AUDIO_ENCODING_SLINEAR_BE, 24, linear24_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_LE, 24, linear24_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_BE, 24, linear24_to_internal);
#else
	TEST("linear24_to_internal");
	XP_SKIP("AUDIO_SUPPORT_LINEAR24 not defined");
#endif
}

void
test_linear32_to_internal()
{
	test_linear_to(AUDIO_ENCODING_SLINEAR_LE, 32, linear32_to_internal);
	test_linear_to(AUDIO_ENCODING_SLINEAR_BE, 32, linear32_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_LE, 32, linear32_to_internal);
	test_linear_to(AUDIO_ENCODING_ULINEAR_BE, 32, linear32_to_internal);
}

// internal_to_linear* のテスト本体
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

	TEST("internal_to_linear%d(%s)", prec, encname(enc));

	stride = prec;
	memset(dst, 0, sizeof(dst));
	for (int i = 0; i < count; i++) {
		src[i] = i + 1;
		if (i % 2 == 1)
			src[i] |= 0x8000;
	}

	// src から答え exp を作成
	e = exp;
	for (int i = 0; i < count; i++) {
		if (enc_is_unsigned(enc)) {
			// unsigned -> signed
			val = (int32_t)(aint_t)(src[i] ^ 0x8000);
		} else {
			val = src[i];
		}

		// length (internal = 16bit)
		switch (stride) {
		 case 8:
			val >>= 8;
			break;
		 case 16:
			break;
		 case 24:
			val <<= 8;
			break;
		 case 32:
			val <<= 16;
			break;
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
test_internal_to_linear8()
{
	test_to_linear(AUDIO_ENCODING_SLINEAR_LE, 8, internal_to_linear8);
	test_to_linear(AUDIO_ENCODING_SLINEAR_BE, 8, internal_to_linear8);
	test_to_linear(AUDIO_ENCODING_ULINEAR_LE, 8, internal_to_linear8);
	test_to_linear(AUDIO_ENCODING_ULINEAR_BE, 8, internal_to_linear8);
}

void
test_internal_to_linear16()
{
	test_to_linear(AUDIO_ENCODING_SLINEAR_LE, 16, internal_to_linear16);
	test_to_linear(AUDIO_ENCODING_SLINEAR_BE, 16, internal_to_linear16);
	test_to_linear(AUDIO_ENCODING_ULINEAR_LE, 16, internal_to_linear16);
	test_to_linear(AUDIO_ENCODING_ULINEAR_BE, 16, internal_to_linear16);
}

void
test_internal_to_linear24()
{
#if defined(AUDIO_SUPPORT_LINEAR24)
	test_to_linear(AUDIO_ENCODING_SLINEAR_LE, 24, internal_to_linear24);
	test_to_linear(AUDIO_ENCODING_SLINEAR_BE, 24, internal_to_linear24);
	test_to_linear(AUDIO_ENCODING_ULINEAR_LE, 24, internal_to_linear24);
	test_to_linear(AUDIO_ENCODING_ULINEAR_BE, 24, internal_to_linear24);
#else
	TEST("internal_to_linear24");
	XP_SKIP("AUDIO_SUPPORT_LINEAR24 not defined");
#endif
}

void
test_internal_to_linear32()
{
	test_to_linear(AUDIO_ENCODING_SLINEAR_LE, 32, internal_to_linear32);
	test_to_linear(AUDIO_ENCODING_SLINEAR_BE, 32, internal_to_linear32);
	test_to_linear(AUDIO_ENCODING_ULINEAR_LE, 32, internal_to_linear32);
	test_to_linear(AUDIO_ENCODING_ULINEAR_BE, 32, internal_to_linear32);
}

// テスト一覧
#define DEF(x)	{ #x, test_ ## x }
struct testtable testtable[] = {
	DEF(linear8_to_internal),
	DEF(linear16_to_internal),
	DEF(linear24_to_internal),
	DEF(linear32_to_internal),
	DEF(internal_to_linear8),
	DEF(internal_to_linear16),
	DEF(internal_to_linear24),
	DEF(internal_to_linear32),
	{ NULL, NULL },
};
