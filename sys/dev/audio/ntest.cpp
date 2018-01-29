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

struct testtable {
	const char *name;
	void (*func)(void);
};

void init();

int debug;
int netbsd;
int props;
int hwfull;
int x68k;
char testname[100];
int testcount;
int failcount;
const char *devaudio = "/dev/audio0";
const char *devsound = "/dev/sound0";
extern struct testtable testtable[];

void __attribute__((__noreturn__))
usage()
{
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
	props = -1;
	hwfull = 0;
	x68k = 0;

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
		printf("\n");
	}
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
		audio_device_t dev;
		int fd;
		int r;

		fd = open(devaudio, O_WRONLY);
		if (fd == -1)
			err(1, "getprops: open: %s", devaudio);
		r = ioctl(fd, AUDIO_GETPROPS, &props);
		if (r == -1)
			err(1, "getprops:AUDIO_GETPROPS");
		hwfull = (props & AUDIO_PROP_FULLDUPLEX) ? 1 : 0;

		r = ioctl(fd, AUDIO_GETDEV, &dev);
		if (r == -1)
			err(1, "getprops:AUDIO_GETDEV");
		if (strcmp(dev.config, "vs") == 0)
			x68k = 1;
		close(fd);
	}
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
	failcount++;
}
#define XP_SKIP()	xp_skip(__LINE__)
void xp_skip(int line)
{
	/* nothing to do */
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

// ai.*.buffer_size が期待通りか調べる
// bool exp が true なら buffer_size の期待値は非ゼロ、
// exp が false なら buffer_size の期待値はゼロ。
#define XP_BUFFSIZE(exp, act)	xp_buffsize(__LINE__, exp, act, #act)
void xp_buffsize(int line, bool exp, int act, const char *varname)
{
	testcount++;
	if (exp) {
		if (act == 0)
			xp_fail(line, "%s expects non-zero but %d", varname, act);
	} else {
		if (act != 0)
			xp_fail(line, "%s expects zero but %d", varname, act);
	}
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

// O_* を PLAY 側がオープンされてるかに変換
int mode2popen[] = {
	0, 1, 1,
};
// O_* を RECORD 側がオープンされてるかに変換
int mode2ropen[] = {
	1, 0, 1,
};

// オープンモードによるオープン直後の状態を調べる
void
test_open_1(void)
{
	struct audio_info ai;
	int fd;
	int r;
	int expmode[] = {
		AUMODE_RECORD,
		AUMODE_PLAY | AUMODE_PLAY_ALL,
		AUMODE_PLAY | AUMODE_PLAY_ALL | AUMODE_RECORD,
	};

	getprops();

	// 再生専用デバイスのテストとか Half はまた
	for (int mode = 0; mode <= 2; mode++) {
		TEST("open_1(%s)", openmodetable[mode]);
		fd = OPEN(devaudio, mode);
		XP_NE(-1, fd);

		memset(&ai, 0, sizeof(ai));
		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
		XP_EQ(0, r);
		XP_EQ(0, ai.play.pause);
		XP_EQ(0, ai.record.pause);
		// O_RDWR でオープンすると最初から Full duplex になっているようだ
		XP_EQ(mode2popen[mode], ai.play.open);
		XP_EQ(mode2ropen[mode], ai.record.open);
		if (hwfull == 0 && mode == O_RDWR) {
			// HW Half で O_RDWR なら PLAY のみになる
			XP_EQ(AUMODE_PLAY | AUMODE_PLAY_ALL, ai.mode);
		} else {
			XP_EQ(expmode[mode], ai.mode);
		}

		if (netbsd <= 8) {
			// N7、N8 では使わないほうのトラックのバッファも常にある
			XP_NE(0, ai.play.buffer_size);
			XP_NE(0, ai.record.buffer_size);
		} else {
			// AUDIO2 では使わないほうのバッファは確保してない
			XP_BUFFSIZE(mode2popen[mode], ai.play.buffer_size);
			XP_BUFFSIZE(mode2ropen[mode], ai.record.buffer_size);
		}

		r = CLOSE(fd);
		XP_EQ(0, r);
	}
}

// /dev/audio は何回開いても初期値は同じ
void
test_open_2(void)
{
	struct audio_info ai, ai0;
	int channels;
	int fd;
	int r;

	TEST("open_2");
	getprops();

	// オープンして初期値をチェック
	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_EQ(0, r);

	XP_NE(0, ai.blocksize);
	XP_NE(0, ai.hiwat);
	XP_NE(0, ai.lowat);
	XP_EQ(AUMODE_PLAY | AUMODE_PLAY_ALL, ai.mode);
	// play
	XP_EQ(8000, ai.play.sample_rate);
	XP_EQ(1, ai.play.channels);
	XP_EQ(8, ai.play.precision);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
	// gain
	// port
	XP_EQ(0, ai.play.seek);
	// avail_ports
	XP_BUFFSIZE(1, ai.play.buffer_size);
	XP_EQ(0, ai.play.samples);
	XP_EQ(0, ai.play.eof);
	XP_EQ(0, ai.play.pause);
	XP_EQ(0, ai.play.error);
	XP_EQ(0, ai.play.waiting);
	// balance
	XP_EQ(1, ai.play.open);
	XP_EQ(0, ai.play.active);
	// record
	XP_EQ(8000, ai.record.sample_rate);
	XP_EQ(1, ai.record.channels);
	XP_EQ(8, ai.record.precision);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.record.encoding);
	// gain
	// port
	XP_EQ(0, ai.record.seek);
	// avail_ports
	XP_BUFFSIZE((netbsd <= 8), ai.record.buffer_size);
	XP_EQ(0, ai.record.samples);
	XP_EQ(0, ai.record.eof);
	XP_EQ(0, ai.record.pause);
	XP_EQ(0, ai.record.error);
	XP_EQ(0, ai.record.waiting);
	// balance
	XP_EQ(0, ai.record.open);
	XP_EQ(0, ai.record.active);
	// これを保存しておく
	ai0 = ai;

	// できるだけ変更
	channels = (netbsd <= 7 && x68k) ? 1 : 2;
	AUDIO_INITINFO(&ai);
	if (ai0.hiwat > 0)
		ai.hiwat = ai0.hiwat - 1;
	if (ai0.lowat < ai0.hiwat)
		ai.lowat = ai0.lowat + 1;
	ai.mode = AUMODE_PLAY;
	ai.play.sample_rate = 11025;
	ai.play.channels = channels;
	ai.play.precision = 16;
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.pause = 1;
	ai.record.sample_rate = 11025;
	ai.record.channels = channels;
	ai.record.precision = 16;
	ai.record.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.record.pause = 1;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "ai");
	if (r == -1)
		err(1, "AUDIO_SETINFO");
	CLOSE(fd);

	// 再オープンしてチェック
	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_EQ(0, r);

	XP_NE(0, ai.blocksize);
	XP_NE(0, ai.hiwat);
	XP_NE(0, ai.lowat);
	XP_EQ(AUMODE_PLAY | AUMODE_PLAY_ALL, ai.mode);
	// play
	XP_EQ(8000, ai.play.sample_rate);
	XP_EQ(1, ai.play.channels);
	XP_EQ(8, ai.play.precision);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
	// gain
	// port
	XP_EQ(0, ai.play.seek);
	// avail_ports
	XP_NE(0, ai.play.buffer_size);
	XP_EQ(0, ai.play.samples);
	XP_EQ(0, ai.play.eof);
	XP_EQ(0, ai.play.pause);
	XP_EQ(0, ai.play.error);
	XP_EQ(0, ai.play.waiting);
	// balance
	XP_EQ(1, ai.play.open);
	XP_EQ(0, ai.play.active);
	// record
	XP_EQ(8000, ai.record.sample_rate);
	XP_EQ(1, ai.record.channels);
	XP_EQ(8, ai.record.precision);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.record.encoding);
	// gain
	// port
	XP_EQ(0, ai.record.seek);
	// avail_ports
	XP_BUFFSIZE((netbsd <= 8), ai.record.buffer_size);
	XP_EQ(0, ai.record.samples);
	XP_EQ(0, ai.record.eof);
	XP_EQ(0, ai.record.pause);
	XP_EQ(0, ai.record.error);
	XP_EQ(0, ai.record.waiting);
	// balance
	XP_EQ(0, ai.record.open);
	XP_EQ(0, ai.record.active);

	CLOSE(fd);
}

// /dev/sound は前回の値がみえる
void
test_open_3(void)
{
	struct audio_info ai, ai0;
	int channels;
	int fd;
	int r;

	TEST("open_3");
	getprops();

	// まず /dev/audio 開いて初期化させておく
	fd = OPEN(devaudio, O_RDONLY);
	if (fd == -1)
		err(1, "open");
	CLOSE(fd);

	// オープンして初期値をチェック
	fd = OPEN(devsound, O_WRONLY);
	if (fd == -1)
		err(1, "open");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_EQ(0, r);

	// audio の初期値と同じものが見えるはず
	XP_NE(0, ai.blocksize);
	// hiwat, lowat
	XP_EQ(AUMODE_PLAY | AUMODE_PLAY_ALL, ai.mode);
	// play
	XP_EQ(8000, ai.play.sample_rate);
	XP_EQ(1, ai.play.channels);
	XP_EQ(8, ai.play.precision);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
	// gain
	// port
	XP_EQ(0, ai.play.seek);
	// avail_ports
	XP_NE(0, ai.play.buffer_size);
	XP_EQ(0, ai.play.samples);
	XP_EQ(0, ai.play.eof);
	XP_EQ(0, ai.play.pause);
	XP_EQ(0, ai.play.error);
	XP_EQ(0, ai.play.waiting);
	// balance
	XP_EQ(1, ai.play.open);
	XP_EQ(0, ai.play.active);
	// record
	XP_EQ(8000, ai.record.sample_rate);
	XP_EQ(1, ai.record.channels);
	XP_EQ(8, ai.record.precision);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.record.encoding);
	// gain
	// port
	XP_EQ(0, ai.record.seek);
	// avail_ports
	XP_BUFFSIZE((netbsd <= 8), ai.record.buffer_size);
	XP_EQ(0, ai.record.samples);
	XP_EQ(0, ai.record.eof);
	XP_EQ(0, ai.record.pause);
	XP_EQ(0, ai.record.error);
	XP_EQ(0, ai.record.waiting);
	// balance
	XP_EQ(0, ai.record.open);
	XP_EQ(0, ai.record.active);

	// できるだけ変更
	channels = (netbsd <= 7 && x68k) ? 1 : 2;
	AUDIO_INITINFO(&ai);
	ai.mode = AUMODE_PLAY_ALL;
	ai.play.sample_rate = 11025;
	ai.play.channels = channels;
	ai.play.precision = 16;
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.pause = 1;
	ai.record.sample_rate = 11025;
	ai.record.channels = channels;
	ai.record.precision = 16;
	ai.record.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.record.pause = 1;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "ai");
	if (r == -1)
		err(1, "AUDIO_SETINFO");
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai0, "ai0");
	if (r == -1)
		err(1, "AUDIO_GETBUFINFO");
	CLOSE(fd);

	// 再オープンしてチェック
	fd = OPEN(devsound, O_WRONLY);
	if (fd == -1)
		err(1, "open");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_EQ(0, r);

	XP_NE(0, ai.blocksize);
	// hiwat, lowat は変化する
	XP_EQ(AUMODE_PLAY | AUMODE_PLAY_ALL, ai.mode);
	// play
	XP_EQ(ai0.play.sample_rate, ai.play.sample_rate);
	XP_EQ(ai0.play.channels, ai.play.channels);
	XP_EQ(ai0.play.precision, ai.play.precision);
	XP_EQ(ai0.play.encoding, ai.play.encoding);
	// gain
	// port
	XP_EQ(0, ai.play.seek);
	// avail_ports
	XP_NE(0, ai.play.buffer_size);
	XP_EQ(0, ai.play.samples);
	XP_EQ(0, ai.play.eof);
	XP_EQ(ai0.play.pause, ai.play.pause);
	XP_EQ(0, ai.play.error);
	XP_EQ(0, ai.play.waiting);
	// balance
	XP_EQ(1, ai.play.open);
	XP_EQ(0, ai.play.active);
	// record
	XP_EQ(ai0.record.sample_rate, ai.record.sample_rate);
	XP_EQ(ai0.record.channels, ai.record.channels);
	XP_EQ(ai0.record.precision, ai.record.precision);
	XP_EQ(ai0.record.encoding, ai.record.encoding);
	// gain
	// port
	XP_EQ(0, ai.record.seek);
	// avail_ports
	XP_BUFFSIZE((netbsd <= 8), ai.record.buffer_size);
	XP_EQ(0, ai.record.samples);
	XP_EQ(0, ai.record.eof);
	XP_EQ(ai0.record.pause, ai.record.pause);
	XP_EQ(0, ai.record.error);
	XP_EQ(0, ai.record.waiting);
	// balance
	XP_EQ(0, ai.record.open);
	XP_EQ(0, ai.record.active);

	CLOSE(fd);
}

// /dev/sound -> /dev/audio -> /dev/sound と開くと2回目の sound は
// audio の設定に影響される。
// /dev/audio と /dev/sound の設定は互いに独立しているわけではなく、
// 内部設定は1つで、
// /dev/sound はそれを継承して使い、/dev/audio はそれを初期化して使う。
// というイメージのようだ。
void
test_open_4(void)
{
	struct audio_info ai, ai0;
	int fd;
	int r;

	TEST("open_4");

	// まず /dev/sound を開いて適当に設定。
	// 代表値でエンコーディングだけ変える。
	// このケースでだけ open_2、open_3 とは挙動が違う項目が一つだけ
	// あるとかだと捕捉できないが、さすがにいいだろう…。
	fd = OPEN(devsound, O_WRONLY);
	if (fd == -1)
		err(1, "open");
	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_SETINFO");
	CLOSE(fd);

	// 続いて /dev/audio をオープン。オープンしただけで自身は mulaw になる
	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");
	CLOSE(fd);

	// 再び /dev/sound をオープン。
	fd = OPEN(devsound, O_WRONLY);
	if (fd == -1)
		err(1, "open");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_GETBUFINFO");
	XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
	CLOSE(fd);
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
		8, 16, 32,
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
					fd = OPEN(devaudio, O_WRONLY);
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
					if (netbsd >= 8) {
						XP_EQ(0, r);
					} else {
						if (r == 0) {
							XP_EQ(0, r);
						} else {
							XP_SKIP();
						}
					}

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
		0, 4, 24,
	};
	for (int i = 0; i < __arraycount(prectable); i++) {
		int prec = prectable[i];
		TEST("encoding_2(prec=%d)", prec);
		fd = OPEN(devaudio, O_WRONLY);
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
		fd = OPEN(devaudio, O_WRONLY);
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

		fd = OPEN(devaudio, O_WRONLY);
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

	TEST("drain_1");
	getprops();

	fd = open(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	struct audio_info ai;
	AUDIO_INITINFO(&ai);
	// 1フレーム複数バイト、PLAY に設定
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 16;
	ai.play.channels = (netbsd <= 7 && x68k) ? 1 : 2;
	ai.play.sample_rate = 11050;
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

	TEST("drain_2");
	getprops();

	fd = open(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	struct audio_info ai;
	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 16;
	ai.play.channels = (netbsd <= 7 && x68k) ? 1 : 2;
	ai.play.sample_rate = 11050;
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
	if (netbsd < 8) {
		XP_SKIP();
		return;
	}

	// 1人目が ASYNC on
	fd0 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 1;
	r = IOCTL(fd0, FIOASYNC, &val, "on");
	XP_EQ(0, r);

	// 続いて2人目が ASYNC on
	fd1 = OPEN(devaudio, O_WRONLY);
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
	if (netbsd < 8) {
		XP_SKIP();
		return;
	}

	// 1人目が ASYNC on
	fd0 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 1;
	r = IOCTL(fd0, FIOASYNC, &val, "on");
	XP_EQ(0, r);

	// 続いて2人目が ASYNC off
	fd1 = OPEN(devaudio, O_WRONLY);
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
	if (netbsd < 8) {
		XP_SKIP();
		return;
	}

	// 1人目がオープン
	fd0 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");

	// 2人目が ASYNC on してクローズ。2人目の ASYNC 状態は無効
	fd1 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 1;
	r = IOCTL(fd1, FIOASYNC, &val, "on");
	XP_EQ(0, r);
	CLOSE(fd1);

	// もう一回2人目がオープンして ASYNC on
	fd1 = OPEN(devaudio, O_WRONLY);
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
// いまいち N7 でシグナルが飛んでくる条件が分からん。
// PLAY_ALL でブロックサイズ書き込んだら飛んでくるようだ。
// PLAY/PLAY_ALL で 4バイト書き込むとかではだめだった。
void
test_FIOASYNC_4(void)
{
	struct audio_info ai;
	int r;
	int fd;
	int val;
	char *data;

	TEST("FIOASYNC_4");
	signal(SIGIO, signal_FIOASYNC_4);
	sigio_caught = 0;

	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	if (r == -1)
		err(1, "ioctl");
	if (ai.blocksize == 0)
		errx(1, "blocksize == 0");
	data = (char *)malloc(ai.blocksize);
	if (data == NULL)
		err(1, "malloc");
	memset(data, 0, ai.blocksize);

	val = 1;
	r = IOCTL(fd, FIOASYNC, &val, "on");
	XP_EQ(0, r);

	r = WRITE(fd, data, ai.blocksize);
	XP_EQ(ai.blocksize, r);

	for (int i = 0; i < 10 && sigio_caught == 0; i++) {
		usleep(10000);
	}
	XP_EQ(1, sigio_caught);

	CLOSE(fd);
	free(data);

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

	fd = OPEN(devaudio, O_RDONLY);
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

	TEST("AUDIO_WSEEK_1");
	getprops();

	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open: %s", devaudio);

	struct audio_info ai;
	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 8;
	ai.play.channels = (netbsd <= 7 && x68k) ? 1 : 2;
	ai.play.sample_rate = 11050;
	ai.play.pause = 1;
	ai.mode = AUMODE_PLAY_ALL;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "pause=1");
	if (r == -1)
		err(1, "AUDIO_SETINFO.pause");

	// 初期状態だと 0 バイトになる
	n = 0;
	r = IOCTL(fd, AUDIO_WSEEK, &n, "");
	if (r == -1)
		err(1, "AUDIO_WSEEK(1)");
	XP_EQ(0, n);

	// 4バイト書き込むと 4になる
	// N7 では 0 になる。要調査。
	r = WRITE(fd, &r, 4);
	if (r == -1)
		err(1, "write(4)");
	r = IOCTL(fd, AUDIO_WSEEK, &n, "");
	if (r == -1)
		err(1, "AUDIO_WSEEK(2)");
	XP_EQ(4, n);

	CLOSE(fd);
}

// SETFD は N7 ならオープンモードに関わらず audio layer の状態を変えるの意
// (で同時に MD の状態も必要なら変える)。GETFD は audio layer の duplex 状態
// を取得するだけ。
// N8 はソースコード上踏襲しているので見た目の動作は同じだが、検討した上での
// それかどうかが謎。

void
test_AUDIO_SETFD_ONLY(void)
{
	struct audio_info ai;
	int r;
	int fd;
	int n;

	getprops();

	for (int mode = 0; mode <= 1; mode++) {
		TEST("AUDIO_SETFD_ONLY(%s)", openmodetable[mode]);

		fd = OPEN(devaudio, mode);
		if (fd == -1)
			err(1, "open: %s", devaudio);

		// オープン直後は常に Half
		n = 0;
		r = IOCTL(fd, AUDIO_GETFD, &n, "");
		XP_EQ(0, r);
		XP_EQ(0, n);

		// Full duplex に設定しようとすると、
		// o N7 では、HW Full なら設定できる、HW Half ならエラー。
		// o AUDIO2 では、オープンモードが RDWR でないのでこれはエラー。
		n = 1;
		r = IOCTL(fd, AUDIO_SETFD, &n, "on");
		if (netbsd <= 8) {
			if (hwfull) {
				XP_EQ(0, r);
			} else {
				XP_EQ(-1, r);
				if (r == -1)
					XP_EQ(ENOTTY, errno);
			}
		} else {
			XP_EQ(-1, r);
			if (r == -1)
				XP_EQ(ENOTTY, errno);
		}

		// 取得してみると、
		// o N7 では、HW Full なら 1、HW Half なら 0 のまま。
		// o AUDIO2 では直近の SETFD がエラーなので変化しない。
		n = 0;
		r = IOCTL(fd, AUDIO_GETFD, &n, "");
		XP_EQ(0, r);
		if (netbsd <= 8) {
			XP_EQ(hwfull, n);
		} else {
			XP_EQ(0, n);
		}

		// GETINFO の ai.*.open などトラック状態は変化しない。
		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
		XP_EQ(0, r);
		XP_EQ(mode2popen[mode], ai.play.open);
		XP_EQ(mode2ropen[mode], ai.record.open);

		// Half duplex に設定しようとすると、
		// o N7 では、HW Full なら設定できる、HW Half なら何も起きず成功。
		n = 0;
		r = IOCTL(fd, AUDIO_SETFD, &n, "off");
		XP_EQ(0, r);

		// 取得してみると、
		// o N7 では、HW Full なら 0、HW Half なら 0 のまま。
		n = 0;
		r = IOCTL(fd, AUDIO_GETFD, &n, "");
		XP_EQ(0, r);
		XP_EQ(0, n);

		// GETINFO の ai.*.open などトラック状態は変化しない。
		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
		XP_EQ(0, r);
		XP_EQ(mode2popen[mode], ai.play.open);
		XP_EQ(mode2ropen[mode], ai.record.open);

		CLOSE(fd);
	}
}

void
test_AUDIO_SETFD_RDWR(void)
{
	struct audio_info ai;
	int r;
	int fd;
	int n;

	TEST("AUDIO_SETFD_RDWR");
	getprops();

	fd = OPEN(devaudio, O_RDWR);
	if (fd == -1)
		err(1, "open: %s", devaudio);

	// O_RDWR オープン直後は
	// HW Full なら Full、
	// HW Half なら Half になる。
	n = 0;
	r = IOCTL(fd, AUDIO_GETFD, &n, "");
	XP_EQ(0, r);
	XP_EQ(hwfull, n);

	// Full duplex に設定しようとすると
	// HW Full なら設定できる(何も起きない)、HW Half ならエラー
	n = 1;
	r = IOCTL(fd, AUDIO_SETFD, &n, "on");
	if (hwfull) {
		XP_EQ(0, r);
	} else {
		XP_EQ(-1, r);
		if (r == -1)
			XP_EQ(ENOTTY, errno);
	}

	// 取得してみると、
	// o N7 では、HW Full なら 1、HW Half なら 0 のまま。
	n = 0;
	r = IOCTL(fd, AUDIO_GETFD, &n, "");
	XP_EQ(0, r);
	XP_EQ(hwfull, n);

	// GETINFO の ai.*.open などトラック状態は変化しない。
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_EQ(0, r);
	XP_EQ(1, ai.play.open);
	XP_EQ(1, ai.record.open);

	// Half duplex に設定しようとすると、
	// o N7 では、HW Full なら設定できる、HW Half なら何も起きず成功。
	// o AUDIO2 では、EINVAL
	n = 0;
	r = IOCTL(fd, AUDIO_SETFD, &n, "off");
	if (netbsd <= 8) {
		XP_EQ(0, r);
	} else {
		XP_EQ(-1, r);
		if (r == -1)
			XP_EQ(EINVAL, errno);
	}

	// 取得してみると、
	// o N7 では、HW Full なら 0、HW Half なら 0 のまま。
	// o AUDIO2 では、直近の SETFD が失敗してるので変化しない。
	n = 0;
	r = IOCTL(fd, AUDIO_GETFD, &n, "");
	XP_EQ(0, r);
	if (netbsd <= 8) {
		XP_EQ(0, n);
	} else {
		XP_EQ(hwfull, n);
	}

	// GETINFO の ai.*.open などトラック状態は変化しない。
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_EQ(0, r);
	XP_EQ(1, ai.play.open);
	XP_EQ(1, ai.record.open);

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
	fd = OPEN(devaudio, O_RDWR);
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

	// 0バイト読んでも上がらない
	r = READ(fd, &r, 0);
	if (r == -1)
		err(1, "read");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_EQ(0, r);
	XP_EQ(2, ai.play.eof);
	XP_EQ(0, ai.record.eof);

	// 別ディスクリプタと干渉しないこと
	if (netbsd >= 8) {
		fd1 = OPEN(devaudio, O_RDWR);
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
	fd = OPEN(devaudio, O_RDWR);
	if (fd == -1)
		err(1, "open");

	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_EQ(0, r);
	XP_EQ(0, ai.play.eof);
	XP_EQ(0, ai.record.eof);

	CLOSE(fd);
}

// SETINFO で mode が切り替わるケース
void
test_AUDIO_SETINFO_mode()
{
	struct audio_info ai;
	int r;
	int fd;
	int n;
	int mode;
	int aumodes[] = {
		AUMODE_RECORD,	// O_RDONLY
		AUMODE_PLAY,	// O_WRONLY
		AUMODE_PLAY | AUMODE_RECORD,	// O_RDWR
	};

	getprops();
	for (int i = 0; i < __arraycount(aumodes); i++) {
		for (int j = 0; j < __arraycount(aumodes); j++) {
			// i が変更前の O_*、j が変更後の O_*
			TEST("AUDIO_SETINFO_mode(%s->%s)",
				openmodetable[i], openmodetable[j]);
			fd = OPEN(devaudio, i);
			if (fd == -1)
				err(1, "open");

			// オープン状態と一致してることが前提
			memset(&ai, 0, sizeof(ai));
			r = IOCTL(fd, AUDIO_GETINFO, &ai, "");
			if (r == -1)
				err(1, "ioctl");
			mode = ai.mode & (AUMODE_PLAY | AUMODE_RECORD);
			if (hwfull == 0 && i == O_RDWR) {
				// HW Half で O_RDWR は AUMODE_PLAY になる
				XP_EQ(AUMODE_PLAY, mode);
			} else {
				XP_EQ(aumodes[i], mode);
			}
			XP_EQ(mode2popen[i], ai.play.open);
			XP_EQ(mode2ropen[i], ai.record.open);
			// N7、N8 では buffer_size は常に非ゼロなので調べない
			if (netbsd >= 9) {
				XP_BUFFSIZE((aumodes[i] & AUMODE_PLAY), ai.play.buffer_size);
				XP_BUFFSIZE((aumodes[i] & AUMODE_RECORD),ai.record.buffer_size);
			}

			// mode を変える
			ai.mode = aumodes[j];
			r = IOCTL(fd, AUDIO_SETINFO, &ai, "mode");
			XP_EQ(0, r);
			if (r == 0) {
				mode = ai.mode & (AUMODE_PLAY | AUMODE_RECORD);
				XP_EQ(aumodes[j], mode);
				// mode に関係なく当初のオープンモードを維持するのかな?
				XP_EQ(mode2popen[i], ai.play.open);
				XP_EQ(mode2ropen[i], ai.record.open);
				// N7、N8 では buffer_size は常に非ゼロなので調べない
				if (netbsd >= 9) {
					XP_BUFFSIZE((aumodes[i] & AUMODE_PLAY),
						ai.play.buffer_size);
					XP_BUFFSIZE((aumodes[i] & AUMODE_RECORD),
						ai.record.buffer_size);
				}
			}

			close(fd);
		}
	}
}

// テスト一覧
#define DEF(x)	{ #x, test_ ## x }
struct testtable testtable[] = {
	DEF(open_1),
	DEF(open_2),
	DEF(open_3),
	DEF(open_4),
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
	DEF(AUDIO_SETINFO_mode),
	{ NULL, NULL },
};

