
/*
 * 実環境での動作テスト用
 */

#include <errno.h>
#include <fcntl.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <poll.h>
#include <soundcard.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <util.h>
#include <sys/atomic.h>
#include <sys/audioio.h>
#include <sys/cdefs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include "../../pad/padio.h"	// PAD_GET_AUDIOUNIT

#if !defined(AUDIO_ENCODING_SLINEAR_NE)
#if BYTE_ORDER == LITTLE_ENDIAN
#define AUDIO_ENCODING_SLINEAR_NE AUDIO_ENCODING_SLINEAR_LE
#define AUDIO_ENCODING_ULINEAR_NE AUDIO_ENCODING_ULINEAR_LE
#define AUDIO_ENCODING_SLINEAR_OE AUDIO_ENCODING_SLINEAR_BE
#define AUDIO_ENCODING_ULINEAR_OE AUDIO_ENCODING_ULINEAR_BE
#else
#define AUDIO_ENCODING_SLINEAR_NE AUDIO_ENCODING_SLINEAR_BE
#define AUDIO_ENCODING_ULINEAR_NE AUDIO_ENCODING_ULINEAR_BE
#define AUDIO_ENCODING_SLINEAR_OE AUDIO_ENCODING_SLINEAR_LE
#define AUDIO_ENCODING_ULINEAR_OE AUDIO_ENCODING_ULINEAR_LE
#endif
#endif

struct testtable {
	const char *name;
	void (*func)(void);
};

void init(int);
void do_test(int);

int debug;
int netbsd;
int props;
int hwfull;
int canplay;
int canrec;
char hwconfig[16];
char hwconfig9[16];	// audio%d
int x68k;
int vs0;
char testname[100];
char descname[100];
int testcount;
int failcount;
int expfcount;
int skipcount;
char devaudio[16];
char devsound[16];
char devaudioctl[16];
char devmixer[16];
extern struct testtable testtable[];

void __attribute__((__noreturn__))
usage()
{
	printf("usage:\t%s [<options>] -a|-c\t\t.. test all\n", getprogname());
	printf("\t%s [<options>] -l <testname>\t.. test <testname> or later\n",
		getprogname());
	printf("\t%s [<options>] <testname...>\t.. test <testname...>\n",
		getprogname());
	printf("options:\n");
	printf("\t-a: test all (exclude concurrent tests)\n");
	printf("\t-d: debug\n");
	printf("\t-u <unit>: audio/sound device unit number (defualt:0)\n");
	printf("testname:\n");
	for (int i = 0; testtable[i].name != NULL; i++) {
		printf("\t%s\n", testtable[i].name);
	}
	exit(1);
}

int
main(int ac, char *av[])
{
	int i;
	int c;
	int opt_all;
	int opt_later;
	int unit;

	props = -1;
	hwfull = 0;
	canplay = 0;
	canrec = 0;
	x68k = 0;
	vs0 = 0;
	unit = 0;

	// global option
	opt_all = 0;
	opt_later = 0;
	while ((c = getopt(ac, av, "adlu:")) != -1) {
		switch (c) {
		 case 'a':
			opt_all = 1;
			break;
		 case 'd':
			debug++;
			break;
		 case 'l':
			opt_later = 1;
			break;
		 case 'u':
			unit = atoi(optarg);
			if (unit < 0) {
				printf("invalid device unit: %d\n", unit);
				exit(1);
			}
			break;
		 default:
			usage();
		}
	}
	ac -= optind;
	av += optind;

	init(unit);

	if (opt_all) {
		// -a なら引数なしで、全項目テスト
		if (ac > 0)
			usage();

		for (int j = 0; testtable[j].name != NULL; j++) {
			if (strncmp(testtable[j].name, "concurrent", 10) != 0)
				do_test(j);
		}
	} else {
		// -a なしなら test
		if (ac == 0)
			usage();

		bool found = false;
		if (opt_later) {
			// -l なら指定された1つ(前方一致)とそれ以降を順にテスト
			// さっきこけたやつ以降を試したいということ。
			if (ac > 1)
				usage();
			int j;
			// 一致しない間スキップ
			for (j = 0; testtable[j].name != NULL; j++) {
				if (strncmp(av[0], testtable[j].name, strlen(av[0])) == 0)
					break;
			}
			// 以降を全部テスト
			for (; testtable[j].name != NULL; j++) {
				do_test(j);
				found = true;
			}
		} else {
			// そうでなければ指定されたやつ(前方一致)を順にテスト
			for (i = 0; i < ac; i++) {
				for (int j = 0; testtable[j].name != NULL; j++) {
					if (strncmp(av[i], testtable[j].name, strlen(av[i])) == 0) {
						do_test(j);
						found = true;
					}
				}
			}
		}
		if (found == false) {
			printf("test not found: %s\n", av[i]);
			exit(1);
		}
	}
	if (testcount > 0) {
		printf("Result: %d tests, %d success",
			testcount,
			testcount - failcount - expfcount - skipcount);
		if (failcount > 0)
			printf(", %d failed", failcount);
		if (expfcount > 0)
			printf(", %d expected failure", expfcount);
		if (skipcount > 0)
			printf(", %d skipped", skipcount);
		printf("\n");
	}
	return 0;
}

#include "xptest.cpp"

// ---

void
test_SNDCTL_DSP_SPEED()
{
	int fd;
	int r;
	int rate;

	TEST("SNDCTL_DSP_SPEED");

	for (int mode = 0; mode <= 2; mode++) {
		DESC("%s", openmodetable[mode]);

		fd = OPEN(devaudio, mode);
		if (fd == -1) {
			// テスト不要
			continue;
		}

		// 検査
		rate = 22050;
		r = IOCTL(fd, SNDCTL_DSP_SPEED, &rate, "");
		XP_SYS_EQ(0, r);
		XP_EQ(22050, rate);

		r = CLOSE(fd);
		XP_SYS_EQ(0, r);
	}
}

void
test_SNDCTL_DSP_STEREO()
{
	int fd;
	int r;
	int is_stereo;

	TEST("SNDCTL_DSP_STEREO");

	for (int mode = 0; mode <= 2; mode++) {
		DESC("%s", openmodetable[mode]);

		fd = OPEN(devaudio, mode);
		if (fd == -1) {
			// テスト不要
			continue;
		}

		// 検査(mono)
		is_stereo = 0;
		r = IOCTL(fd, SNDCTL_DSP_STEREO, &is_stereo, "stereo=0");
		XP_SYS_EQ(0, r);
		XP_EQ(0, is_stereo);

		// 検査(stereo)
		// 非ゼロなら stereo のはず
		is_stereo = 100;
		r = IOCTL(fd, SNDCTL_DSP_STEREO, &is_stereo, "stereo=1");
		XP_SYS_EQ(0, r);
		// 非ゼロかだけ調べる
		DPRINTF("  > %d: is_stereo = %d\n", __LINE__, is_stereo);
		XP_NE(0, is_stereo);

		r = CLOSE(fd);
		XP_SYS_EQ(0, r);
	}
}

void
test_SNDCTL_DSP_SETFMT()
{
	int fd;
	int r;
	int fmt;

	TEST("SNDCTL_DSP_SETFMT");

	for (int mode = 0; mode <= 2; mode++) {
		DESC("%s", openmodetable[mode]);

		fd = OPEN(devaudio, mode);
		if (fd == -1) {
			// テスト不要
			continue;
		}

		// 検査
		// とりあえず代表で一つだけ
		fmt = AFMT_S16_LE;
		r = IOCTL(fd, SNDCTL_DSP_SETFMT, &fmt, "");
		XP_SYS_EQ(0, r);
		DPRINTF("  > %d: fmt = %d\n", __LINE__, fmt);
		XP_EQ(AFMT_S16_LE, fmt);

		r = CLOSE(fd);
		XP_SYS_EQ(0, r);
	}
}

void
test_SNDCTL_DSP_CHANNELS()
{
	int fd;
	int r;
	int channels;

	TEST("SNDCTL_DSP_CHANNELS");

	for (int mode = 0; mode <= 2; mode++) {
		DESC("%s", openmodetable[mode]);

		fd = OPEN(devaudio, mode);
		if (fd == -1) {
			// テスト不要
			continue;
		}

		// 検査
		// とりあえず代表で一つだけ
		channels = 2;
		r = IOCTL(fd, SNDCTL_DSP_CHANNELS, &channels, "");
		XP_SYS_EQ(0, r);
		DPRINTF("  > %d: channels = %d\n", __LINE__, channels);
		XP_EQ(2, channels);

		r = CLOSE(fd);
		XP_SYS_EQ(0, r);
	}
}


// テスト一覧
#define DEF(x)	{ #x, test_ ## x }
struct testtable testtable[] = {
	DEF(SNDCTL_DSP_SPEED),
	DEF(SNDCTL_DSP_STEREO),
	DEF(SNDCTL_DSP_SETFMT),
	DEF(SNDCTL_DSP_CHANNELS),
	{ NULL, NULL },
};
