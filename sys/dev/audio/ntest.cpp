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
int props;
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
	props = -1;

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
			testname[0] = '\0';
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

		// そうでなければ指定されたやつ(前方一致)を順にテスト
		for (i = 0; i < ac; i++) {
			for (int j = 0; testtable[j].name != NULL; j++) {
				if (strncmp(av[i], testtable[j].name, strlen(av[i])) == 0) {
					testtable[j].func();
					testname[0] = '\0';
				}
			}
		}
	}
	printf("Result: %d tests, %d success", testcount, testcount - failcount);
	if (failcount > 0)
		printf(", %d failed", failcount);
	printf("\n");
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

// デバイスのプロパティを取得します。
int
getprops()
{
	// GETPROPS のための open/ioctl/close が信頼できるかどうかは悩ましいが
	// とりあえずね
	if (props == -1) {
		int fd;
		int r;

		fd = open(devicename, O_WRONLY);
		if (fd == -1)
			err(1, "getprops: open: %s", devicename);
		r = ioctl(fd, AUDIO_GETPROPS, &props);
		if (r == -1)
			err(1, "getprops:AUDIO_GETPROPS");
		close(fd);
	}
}

// テスト名
#define TEST(name...)	do {	\
	snprintf(testname, sizeof(testname), name);	\
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
	testcount++;
	if (exp != act)
		xp_fail(line, "expects %d but %d", exp, act);
}
#define XP_NE(exp, act)	xp_ne(__LINE__, exp, act)
void xp_ne(int line, int exp, int act)
{
	testcount++;
	if (exp == act)
		xp_fail(line, "expects != %d but %d", exp, act);
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

#define READ(fd, addr, len)	debug_read(__LINE__, fd, addr, len)
int debug_read(int line, int fd, void *addr, size_t len)
{
	DPRINTFF(line, "read(%d, %p, %zd)", fd, addr, len);
	int r = read(fd, addr, len);
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
	struct audio_info ai;
	int fd;
	int r;

	// 再生専用デバイスのテストとか Half はまた
	for (int mode = 0; mode <= 2; mode++) {
		TEST("open_%s", openmodetable[mode]);
		fd = OPEN(devicename, mode);
		XP_NE(-1, fd);

		memset(&ai, 0, sizeof(ai));
		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
		XP_EQ(0, r);
		XP_EQ(0, ai.play.pause);
		XP_EQ(0, ai.record.pause);

		r = CLOSE(fd);
		XP_EQ(0, r);
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

// pause したまま drain
void
test_drain_2(void)
{
	int r;
	int fd;

	fd = open(devicename, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	TEST("drain_1");
	struct audio_info ai;
	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 16;
	ai.play.channels = 2;
	ai.play.sample_rate = 44100;
	ai.mode = AUMODE_PLAY;
	ai.play.pause = 1;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_SETINFO");
	// 4バイト書いて close
	r = WRITE(fd, &r, 4);
	XP_EQ(4, r);
	r = CLOSE(fd);
	XP_EQ(0, r);
}

// FIOASYNC が同時に2人設定できるか
void
test_FIOASYNC_1(void)
{
	int r;
	int fd0, fd1;
	int val;

	TEST("FIOASYNC_1");

	// 1人目が ASYNC on
	fd0 = OPEN(devicename, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 1;
	r = IOCTL(fd0, FIOASYNC, &val, "on");
	XP_EQ(0, r);

	// 続いて2人目が ASYNC on
	fd1 = OPEN(devicename, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 1;
	r = IOCTL(fd1, FIOASYNC, &val, "on");
	XP_EQ(0, r);

	CLOSE(fd0);
	CLOSE(fd1);
}

// FIOASYNC が別トラックに影響を与えないこと
void
test_FIOASYNC_2(void)
{
	int r;
	int fd0, fd1;
	int val;

	TEST("FIOASYNC_2");

	// 1人目が ASYNC on
	fd0 = OPEN(devicename, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 1;
	r = IOCTL(fd0, FIOASYNC, &val, "on");
	XP_EQ(0, r);

	// 続いて2人目が ASYNC off
	fd1 = OPEN(devicename, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 0;
	r = IOCTL(fd1, FIOASYNC, &val, "off");
	XP_EQ(0, r);

	CLOSE(fd0);
	CLOSE(fd1);
}

// FIOASYNC リセットのタイミング
void
test_FIOASYNC_3(void)
{
	int r;
	int fd0, fd1;
	int val;

	TEST("FIOASYNC_3");

	// 1人目がオープン
	fd0 = OPEN(devicename, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");

	// 2人目が ASYNC on してクローズ。2人目の ASYNC 状態は無効
	fd1 = OPEN(devicename, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 1;
	r = IOCTL(fd1, FIOASYNC, &val, "on");
	XP_EQ(0, r);
	CLOSE(fd1);

	// もう一回2人目がオープンして ASYNC on
	fd1 = OPEN(devicename, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 1;
	r = IOCTL(fd1, FIOASYNC, &val, "on");
	XP_EQ(0, r);
	CLOSE(fd1);
	CLOSE(fd0);
}

volatile int sigio_caught;
void
signal_FIOASYNC_4(int signo)
{
	if (signo == SIGIO)
		sigio_caught = 1;
}

// 書き込みで SIGIO が飛んでくるか
void
test_FIOASYNC_4(void)
{
	int r;
	int fd;
	int val;

	TEST("FIOASYNC_4");
	signal(SIGIO, signal_FIOASYNC_4);
	sigio_caught = 0;

	fd = OPEN(devicename, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	val = 1;
	r = IOCTL(fd, FIOASYNC, &val, "on");
	XP_EQ(0, r);

	r = WRITE(fd, &r, 4);
	XP_EQ(4, r);

	for (int i = 0; i < 10 && sigio_caught == 0; i++) {
		usleep(10000);
	}
	XP_EQ(1, sigio_caught);

	CLOSE(fd);

	signal(SIGIO, SIG_IGN);
	sigio_caught = 0;
}

// 録音で SIGIO が飛んでくるか
void
test_FIOASYNC_5(void)
{
	int r;
	int fd;
	int val;

	TEST("FIOASYNC_5");
	signal(SIGIO, signal_FIOASYNC_4);
	sigio_caught = 0;

	fd = OPEN(devicename, O_RDONLY);
	if (fd == -1)
		err(1, "open");

	val = 1;
	r = IOCTL(fd, FIOASYNC, &val, "on");
	XP_EQ(0, r);

	r = READ(fd, &r, 4);
	XP_EQ(4, r);

	for (int i = 0; i < 10 && sigio_caught == 0; i++) {
		usleep(10000);
	}
	XP_EQ(1, sigio_caught);

	CLOSE(fd);

	signal(SIGIO, SIG_IGN);
	sigio_caught = 0;
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

void
test_AUDIO_SETFD_ONLY(void)
{
	int r;
	int fd;
	int n;
	int hwfull;

	// プロパティから Full/Half を知る
	getprops();
	hwfull = (props & AUDIO_PROP_FULLDUPLEX) ? 1 : 0;

	// O_xxONLY でオープンすると GETFD == 0 になる
	for (int mode = 0; mode <= 1; mode++) {
		TEST("AUDIO_SETFD_ONLY_%s", openmodetable[mode]);

		fd = OPEN(devicename, mode);
		if (fd == -1)
			err(1, "open: %s", devicename);

		// オープン直後は常に Half
		n = 0;
		r = IOCTL(fd, AUDIO_GETFD, &n, "");
		XP_EQ(0, r);
		XP_EQ(0, n);

		// でも Full には変更できる (O_xxONLY はあくまで初期値?)
		n = 1;
		r = IOCTL(fd, AUDIO_SETFD, &n, "on");
		if (hwfull) {
			XP_EQ(0, r);
		} else {
			XP_EQ(-1, r);
			if (r == -1)
				XP_EQ(ENOTTY, errno);
		}

		// HW Full Duplex なら変更できていること
		// HW Half Duplex なら変更できていないこと
		n = 0;
		r = IOCTL(fd, AUDIO_GETFD, &n, "");
		XP_EQ(0, r);
		XP_EQ(hwfull, n);

		CLOSE(fd);
	}
}

void
test_AUDIO_SETFD_RDWR(void)
{
	int r;
	int fd;
	int n;
	int hwfull;

	getprops();
	hwfull = (props & AUDIO_PROP_FULLDUPLEX) ? 1 : 0;

	TEST("AUDIO_GETFD_RDWR");
	fd = OPEN(devicename, O_RDWR);
	if (fd == -1)
		err(1, "open: %s", devicename);

	// O_RDWR でオープンだと
	// HW full duplex なら FULL
	// HW half duplex なら Half になる
	n = 0;
	r = IOCTL(fd, AUDIO_GETFD, &n, "");
	XP_EQ(0, r);
	XP_EQ(hwfull, n);

	// HW が Full なら Full に切り替え可能
	n = 1;
	r = IOCTL(fd, AUDIO_SETFD, &n, "on");
	if (hwfull) {
		XP_EQ(0, r);
	} else {
		XP_EQ(-1, r);
		if (r == -1)
			XP_EQ(ENOTTY, errno);
	}

	// 切り替えたあとの GETFD
	n = 0;
	r = IOCTL(fd, AUDIO_GETFD, &n, "");
	XP_EQ(0, r);
	XP_EQ(hwfull, n);

	CLOSE(fd);
}

void
test_AUDIO_GETINFO_eof(void)
{
	struct audio_info ai;
	int r;
	int fd, fd1;
	int n;

	TEST("AUDIO_GETINFO_eof");
	fd = OPEN(devicename, O_RDWR);
	if (fd == -1)
		err(1, "open");

	// 最初は 0
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_EQ(0, r);
	XP_EQ(0, ai.play.eof);
	XP_EQ(0, ai.record.eof);

	// 0バイト書き込むと上がる
	r = WRITE(fd, &r, 0);
	if (r == -1)
		err(1, "write");
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_EQ(0, r);
	XP_EQ(1, ai.play.eof);
	XP_EQ(0, ai.record.eof);

	// 1バイト以上を書き込んでも上がらない
	r = WRITE(fd, &r, 4);
	if (r == -1)
		err(1, "write");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_EQ(0, r);
	XP_EQ(1, ai.play.eof);
	XP_EQ(0, ai.record.eof);

	// もう一度0バイト書き込むと上がる
	r = WRITE(fd, &r, 0);
	if (r == -1)
		err(1, "write");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_EQ(0, r);
	XP_EQ(2, ai.play.eof);
	XP_EQ(0, ai.record.eof);

	// 別ディスクリプタと干渉しないこと
	if (netbsd >= 8) {
		fd1 = OPEN(devicename, O_RDWR);
		if (fd1 == -1)
			err(1, "open");
		memset(&ai, 0, sizeof(ai));
		r = IOCTL(fd1, AUDIO_GETBUFINFO, &ai, "");
		XP_EQ(0, r);
		XP_EQ(0, ai.play.eof);
		XP_EQ(0, ai.record.eof);
		CLOSE(fd1);
	}

	CLOSE(fd);

	// オープンしなおすとリセット
	fd = OPEN(devicename, O_RDWR);
	if (fd == -1)
		err(1, "open");

	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_EQ(0, r);
	XP_EQ(0, ai.play.eof);
	XP_EQ(0, ai.record.eof);

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
	DEF(drain_2),
	DEF(FIOASYNC_1),
	DEF(FIOASYNC_2),
	DEF(FIOASYNC_3),
	DEF(FIOASYNC_4),
	DEF(FIOASYNC_5),
	DEF(AUDIO_WSEEK_1),
	DEF(AUDIO_SETFD_ONLY),
	DEF(AUDIO_SETFD_RDWR),
	DEF(AUDIO_GETINFO_eof),
	{ NULL, NULL },
};

