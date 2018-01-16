/*
 * 実環境での動作テスト用
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/audioio.h>
#include <sys/cdefs.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/sysctl.h>

struct cmdtable {
	const char *name;
	int (*func)(int, char *[]);
};
struct testtable {
	const char *name;
	void (*func)(void);
};

void init();

int debug;
int netbsd;
char testname[100];
int testcount;
int failcount;
const char *devicename;
extern struct cmdtable cmdtable[];
extern struct testtable testtable[];

void __attribute__((__noreturn__))
usage()
{
	// cmd は一度に1つ。任意の引数あり。
	printf(" %s <cmd> [<arg...>]\n", getprogname());
	for (int i = 0; cmdtable[i].name != NULL; i++) {
		printf("\t%s\n", cmdtable[i].name);
	}
	// test は複数列挙できる。
	printf(" %s {-a | <testname...>}\n", getprogname());
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

	testname[0] = '\0';
	devicename = "/dev/sound0";

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

	init();

	if (opt_all) {
		// -a なら引数なしで、全項目テスト
		if (ac > 0)
			usage();

		for (int j = 0; testtable[j].name != NULL; j++) {
			testtable[j].func();
		}
	} else {
		// -a なしなら cmd か test
		if (ac == 0)
			usage();

		// 先頭が cmd なら一つだけ受け取って処理
		for (int j = 0; cmdtable[j].name != NULL; j++) {
			if (strcmp(av[0], cmdtable[j].name) == 0) {
				cmdtable[j].func(ac, av);
				return 0;
			}
		}

		// そうでなければ指定されたやつを順にテスト
		for (i = 0; i < ac; i++) {
			for (int j = 0; testtable[j].name != NULL; j++) {
				if (strcmp(av[i], testtable[j].name) == 0) {
					testtable[j].func();
				}
			}
		}
	}
	printf("Result: %d tests, %d success, %d failed\n",
		testcount, testcount - failcount, failcount);
	return 0;
}

void
init()
{
	char name[256];
	size_t len;
	int r;
	int rel;

	// バージョンを適当に判断。
	// 7 系なら 7
	// 8 系なら 8
	// 8.99 系で AUDIO2 なら 9
	// 8.99 系でそれ以外なら 8
	len = sizeof(rel);
	r = sysctlbyname("kern.osrevision", &rel, &len, NULL, 0);
	if (r == -1)
		err(1, "sysctl: osrevision");
	netbsd = rel / 100000000;
	if (netbsd == 8) {
		len = sizeof(name);
		r = sysctlbyname("kern.version", name, &len, NULL, 0);
		if (r == -1)
			err(1, "sysctl: version");
		if (strstr(name, "AUDIO2"))
			netbsd++;
	}

	if (debug)
		printf("netbsd = %d\n", netbsd);
}

// テスト名
#define TEST(name...)	do {	\
	snprintf(testname, sizeof(testname), name);	\
	testcount++;	\
	printf("%s\n", testname);	\
} while (0)

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
	failcount++;
}
#define XP_EQ(exp, act)	xp_eq(__LINE__, exp, act)
void xp_eq(int line, int exp, int act)
{
	if (exp != act)
		xp_fail(line, "expects %d but %d", exp, act);
}

#define DPRINTF(fmt...)	do {	\
	if (debug)	\
		printf(fmt);	\
} while (0)

#define DPRINTFF(line, fmt...)	do {	\
	if (debug) {	\
		printf("  > %d: ", line);	\
		DPRINTF(fmt);	\
		fflush(stdout);	\
	}	\
} while (0)

#define DRESULT(r)	do {	\
	int backup_errno = errno;	\
	if ((r) == -1) {	\
		DPRINTF(" = %d, err#%d %s\n",	\
			r, backup_errno, strerror(backup_errno));	\
	} else {	\
		DPRINTF(" = %d\n", r);	\
	}	\
	errno = backup_errno;	\
	return r;	\
} while (0)

static const char *openmodetable[] = {
	"O_RDONLY",
	"O_WRONLY",
	"O_RDWR",
};

// システムコールはこのマクロを経由して呼ぶ
#define OPEN(name, mode)	debug_open(__LINE__, name, mode)
int debug_open(int line, const char *name, int mode)
{
	if (0 <= mode && mode <= 2)
		DPRINTFF(line, "open(\"%s\", %s)", name, openmodetable[mode]);
	else
		DPRINTFF(line, "open(\"%s\", %d)", name, mode);
	int r = open(name, mode);
	DRESULT(r);
}

#define WRITE(fd, addr, len)	debug_write(__LINE__, fd, addr, len)
int debug_write(int line, int fd, const void *addr, size_t len)
{
	DPRINTFF(line, "write(%d, %p, %zd)", fd, addr, len);
	int r = write(fd, addr, len);
	DRESULT(r);
}

// addrstr は値についてのコメント。ex.
//	int onoff = 0;
//	ioctl(fd, SWITCH, onoff); -> IOCTL(fd, SWITCH, onoff, "off")
#define IOCTL(fd, name, addr, addrstr)	\
	debug_ioctl(__LINE__, fd, name, #name, addr, addrstr)
int debug_ioctl(int line, int fd, u_long name, const char *namestr,
	void *addr, const char *addrstr)
{
	DPRINTFF(line, "ioctl(%d, %s, %s)", fd, namestr, addrstr);
	int r = ioctl(fd, name, addr);
	DRESULT(r);
}

#define CLOSE(fd)	debug_close(__LINE__, fd)
int debug_close(int line, int fd)
{
	DPRINTFF(line, "close(%d)", fd);
	int r = close(fd);
	DRESULT(r);
}

// ---

// ioctl FIONREAD の引数が NULL だったら何がおきるか。
// -> ioctl 上位(kern/sys_generic.c) が領域を用意してそこに書き込んでから
// copyout で転送するため、NULL は直接渡ってこないようだ。
int
cmd_FIONREAD_null(int ac, char *av[])
{
	int fd = OPEN(devicename, O_RDWR);
	if (fd == -1)
		err(1, "open: %s", devicename);

	int r = IOCTL(fd, FIONREAD, NULL, "NULL");
	if (r == -1)
		err(1, "FIONREAD");

	CLOSE(fd);
	return 0;
}

// FIOASYNC の動作を確認する。
// N8 だと一人が FIOASYNC 設定してると2人目は EBUSY らしいが、
// 前の人のは解除できるような気がする。
int
cmd_FIOASYNC(int ac, char *av[])
{
	int r;
	int fd;
	int n;

	if (ac < 2)
		errx(1, "usage: FIOASYNC <0/1>");

	n = atoi(av[1]);
	printf("FIOASYNC %d\n", n);

	fd = OPEN(devicename, O_WRONLY);
	if (fd == -1)
		err(1, "open: %s", devicename);

	r = IOCTL(fd, FIOASYNC, &n, av[1]);
	if (r == -1)
		err(1, "FIOASYNC(%d)", n);

	printf("Hit any key to close\n");
	getchar();

	CLOSE(fd);
	return 0;
}

// open/close
void
test_open_1(void)
{
	int fd;
	int r;

	// 再生専用デバイスのテストとか Half はまた
	for (int mode = 0; mode <= 2; mode++) {
		TEST("open_%s", openmodetable[mode]);
		fd = OPEN(devicename, mode);
		CLOSE(fd);
	}
}

// SETINFO の受付エンコーディング
// 正しく変換できるかどうかまではここでは調べない。
void
test_encoding_1(void)
{
	int fd;
	int r;

	// リニア、正常系
	int enctable[] = {
		AUDIO_ENCODING_SLINEAR_LE,
		AUDIO_ENCODING_SLINEAR_BE,
		AUDIO_ENCODING_ULINEAR_LE,
		AUDIO_ENCODING_ULINEAR_BE,
		AUDIO_ENCODING_SLINEAR,		// for backward compatibility
		AUDIO_ENCODING_ULINEAR,		// for backward compatibility
	};
	int prectable[] = {
		8, 16, 24, 32,
	};
	int chtable[] = {
		1, 2, 4, 8, 12,
	};
	int freqtable[] = {
		1000, 192000,
	};
	for (int i = 0; i < __arraycount(enctable); i++) {
		int enc = enctable[i];
		for (int j = 0; j < __arraycount(prectable); j++) {
			int prec = prectable[j];

			// 実際には HW 依存だが確認方法がないので
			// とりあえず手元の hdafg(4) で動く組み合わせだけ
			if (prec >= 24 && netbsd <= 7)
				continue;

			for (int k = 0; k < __arraycount(chtable); k++) {
				int ch = chtable[k];

				// 実際には HW 依存だが確認方法がないので
				if (ch > 1 && netbsd <= 7)
					continue;

				for (int m = 0; m < __arraycount(freqtable); m++) {
					int freq = freqtable[m];

					char buf[100];
					snprintf(buf, sizeof(buf), "enc=%d,prec=%d,ch=%d,freq=%d",
						enc, prec, ch, freq);

					TEST("encoding_1(%s)", buf);
					fd = OPEN(devicename, O_WRONLY);
					if (fd == -1)
						err(1, "open");

					struct audio_info ai;
					AUDIO_INITINFO(&ai);
					ai.play.encoding = enc;
					ai.play.precision = prec;
					ai.play.channels = ch;
					ai.play.sample_rate = freq;
					ai.mode = AUMODE_PLAY_ALL;
					r = IOCTL(fd, AUDIO_SETINFO, &ai, "play");
					XP_EQ(0, r);

					CLOSE(fd);
				}
			}
		}
	}
}

void
test_encoding_2()
{
	int fd;
	int r;

	// リニア、異常系

	// 本当はサポートしている以外全部なんだが
	int prectable[] = {
		0, 4,
	};
	for (int i = 0; i < __arraycount(prectable); i++) {
		int prec = prectable[i];
		TEST("encoding_2(prec=%d)", prec);
		fd = OPEN(devicename, O_WRONLY);
		if (fd == -1)
			err(1, "open");

		struct audio_info ai;
		AUDIO_INITINFO(&ai);
		ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
		ai.play.precision = prec;
		ai.play.channels = 1;
		ai.play.sample_rate = 1000;
		ai.mode = AUMODE_PLAY_ALL;
		r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
		XP_EQ(-1, r);
		if (r == -1)
			XP_EQ(EINVAL, errno);

		CLOSE(fd);
	}

	int chtable[] = {
		0, 13,
	};
	for (int i = 0; i < __arraycount(chtable); i++) {
		int ch = chtable[i];
		TEST("encoding_2(ch=%d)", ch);
		fd = OPEN(devicename, O_WRONLY);
		if (fd == -1)
			err(1, "open");

		struct audio_info ai;
		AUDIO_INITINFO(&ai);
		ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
		ai.play.precision = 16;
		ai.play.channels = ch;
		ai.play.sample_rate = 1000;
		ai.mode = AUMODE_PLAY_ALL;
		r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
		XP_EQ(-1, r);
		if (r == -1)
			XP_EQ(EINVAL, errno);

		CLOSE(fd);
	}

	int freqtable[] = {
		0, 999,	192001,
	};
	for (int i = 0; i < __arraycount(freqtable); i++) {
		int freq = freqtable[i];
		TEST("encoding_3(freq=%d)", freq);

		// XXX freq=0 は NetBSD<=8 ではプロセスが無限ループに入ってしまう
		if (freq == 0 && netbsd <= 8) {
			XP_FAIL("not tested: it causes infinate loop");
			continue;
		}
		// N7 は周波数のチェックも MI レベルではしない (ただ freq 0 はアカン)
		// N8 はチェックすべきだと思うけどシラン
		if (netbsd <= 8)
			continue;

		fd = OPEN(devicename, O_WRONLY);
		if (fd == -1)
			err(1, "open");

		struct audio_info ai;
		AUDIO_INITINFO(&ai);
		ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
		ai.play.precision = 16;
		ai.play.channels = 2;
		ai.play.sample_rate = freq;
		ai.mode = AUMODE_PLAY_ALL;
		r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
		XP_EQ(-1, r);
		if (r == -1)
			XP_EQ(EINVAL, errno);

		CLOSE(fd);
	}
}

void
test_drain_1(void)
{
	int r;
	int fd;

	fd = open(devicename, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	TEST("drain_1");
	// 1フレーム4バイト、PLAY に設定
	struct audio_info ai;
	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 16;
	ai.play.channels = 2;
	ai.play.sample_rate = 44100;
	ai.mode = AUMODE_PLAY;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_SETINFO");
	// 1バイト書いて close
	r = WRITE(fd, &r, 1);
	XP_EQ(1, r);
	r = CLOSE(fd);
	XP_EQ(0, r);
}

// AUDIO_WSEEK の動作確認
void
test_AUDIO_WSEEK_1(void)
{
	int r;
	int fd;
	int n;

	fd = OPEN(devicename, O_WRONLY);
	if (fd == -1)
		err(1, "open: %s", devicename);

	struct audio_info ai;
	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 8;
	ai.play.channels = 2;
	ai.play.sample_rate = 44100;
	ai.play.pause = 1;
	ai.mode = AUMODE_PLAY_ALL;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "pause=1");
	if (r == -1)
		err(1, "AUDIO_SETINFO.pause");

	// 初期状態だと 0 バイトになる
	TEST("AUDIO_WSEEK_0bytes");
	n = 0;
	r = IOCTL(fd, AUDIO_WSEEK, &n, "");
	if (r == -1)
		err(1, "AUDIO_WSEEK(1)");
	XP_EQ(0, n);

	// 4バイト書き込むと 4になる
	// N7 では 0 になる。要調査。
	TEST("AUDIO_WSEEK_4bytes");
	r = WRITE(fd, &r, 4);
	if (r == -1)
		err(1, "write(4)");
	r = IOCTL(fd, AUDIO_WSEEK, &n, "");
	if (r == -1)
		err(1, "AUDIO_WSEEK(2)");
	XP_EQ(4, n);

	CLOSE(fd);
}

// コマンド一覧
#define DEF(x)	{ #x, cmd_ ## x }
struct cmdtable cmdtable[] = {
	DEF(FIONREAD_null),
	DEF(FIOASYNC),
	{ NULL, NULL },
};
#undef DEF

// テスト一覧
#define DEF(x)	{ #x, test_ ## x }
struct testtable testtable[] = {
	DEF(open_1),
	DEF(encoding_1),
	DEF(encoding_2),
	DEF(drain_1),
	DEF(AUDIO_WSEEK_1),
	{ NULL, NULL },
};

